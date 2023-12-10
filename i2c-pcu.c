// SPDX-License-Identifier: GPL-2.0
/*
 * Intel Skylake-X PCU SMBus driver
 *
 * Copyright (C) 2023 Florin9doi
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/i2c.h>

#define PCI_DEVICE_ID_INTEL_SKLX_PCU    0x2085
#define NR_BUSES                        3

#define SMB_CMD_CFG(b)                  (0x9C + 4*(b))
    #define SMB_CKOVRD                  (1 << 29)
    #define SMB_CMD_TRIGGER             (1 << 19)
    #define SMB_WORD_ACCESS             (1 << 17)
    #define SMB_WRT                     (1 << 15)
    #define SMB_SA                      (1 <<  8)
    #define SMB_SA_SHIFT                8
    #define SMB_SA_MASK                 0x7F00
#define SMB_STATUS_CFG(b)               (0xA8 + 4*(b))
    #define SMB_WOD                     (1 <<  3)
    #define SMB_RDO                     (1 <<  2)
    #define SMB_SBE                     (1 <<  1)
    #define SMB_BUSY                    (1 <<  0)
#define SMB_DATA_CFG(b)                 (0xB4 + 4*(b))
    #define SMB_WDATA_SHIFT             16
    #define SMB_RDATA_MASK_BYTE         0x00FF
    #define SMB_RDATA_MASK_WORD_MSB     0xFF00
    #define SMB_RDATA_MASK_WORD_LSB     0x00FF

struct imc_priv {
    struct pci_dev *pci_dev;
    struct i2c_adapter bus[NR_BUSES];
    bool suspended;
};

static u32 pcu_smb_func(struct i2c_adapter *adapter)
{
    return I2C_FUNC_SMBUS_QUICK
         | I2C_FUNC_SMBUS_BYTE
         | I2C_FUNC_SMBUS_BYTE_DATA
         | I2C_FUNC_SMBUS_WORD_DATA
         | I2C_FUNC_SMBUS_BLOCK_DATA;
}

static s32 pcu_smb_smbus_xfer(struct i2c_adapter *adapter, u16 address,
        unsigned short flags, char read_write, u8 command,
        int size, union i2c_smbus_data *data)
{
    int ret, bus;
    u32 cmd = 0, val = 0, status;
    int i, len = 0, cnt;
    struct imc_priv *priv = i2c_get_adapdata(adapter);

    bus = (adapter == &priv->bus[0] ? 0
        :  adapter == &priv->bus[1] ? 1
        : 2);

    if (read_write == I2C_SMBUS_WRITE) {
        if (0x50 <= address && address <= 0x57) {
            return -EOPNOTSUPP;
        }
        cmd |= SMB_WRT;
        switch (size) {
        case I2C_SMBUS_QUICK:
            break;
        case I2C_SMBUS_BYTE_DATA:
            val = data->byte << SMB_WDATA_SHIFT;
        case I2C_SMBUS_BYTE:
            pci_write_config_dword(priv->pci_dev, SMB_DATA_CFG(bus), val);
            break;
        case I2C_SMBUS_WORD_DATA:
            val = ((data->word & 0xFF00) >> 8 | (data->word & 0x00FF) << 8) << SMB_WDATA_SHIFT;
            pci_write_config_dword(priv->pci_dev, SMB_DATA_CFG(bus), val);
            break;
        case I2C_SMBUS_BLOCK_DATA:
            len = data->block[0];
            if (len == 0 || len > I2C_SMBUS_BLOCK_MAX)
                return -EINVAL;

            val = len << SMB_WDATA_SHIFT;
            pci_write_config_dword(priv->pci_dev, SMB_DATA_CFG(bus), val);
            cnt = 1;
            break;
        default:
            return -EOPNOTSUPP;
        }
    }

    cmd |= SMB_CKOVRD;
    cmd |= SMB_CMD_TRIGGER;
    if (size == I2C_SMBUS_WORD_DATA) {
        cmd |= SMB_WORD_ACCESS;
    }
    cmd |= (address << SMB_SA_SHIFT) & SMB_SA_MASK;
    cmd |= command;
    pci_write_config_dword(priv->pci_dev, SMB_CMD_CFG(bus), cmd);

    do {
        do {
            ret = pci_read_config_dword(priv->pci_dev, SMB_STATUS_CFG(bus), &status);
            if (ret) {
                return -EIO;
            }
            usleep_range(250, 500);
        } while (status & SMB_BUSY);

        if (status & SMB_SBE) {
            // dev_warn(&priv->pci_dev->dev, "SMB_SBE\n");
            return -EIO;
        }

        if (read_write != I2C_SMBUS_WRITE || size != I2C_SMBUS_BLOCK_DATA || cnt >= len)
            break;

        /* Send next byte from the block */
        val = data->block[cnt] << SMB_WDATA_SHIFT;
        pci_write_config_dword(priv->pci_dev, SMB_DATA_CFG(bus), val);
        cnt++;
    } while (size == I2C_SMBUS_BLOCK_DATA && cnt <= len);


    if (read_write == I2C_SMBUS_WRITE) {
        return (status & SMB_WOD ? 0 : -EIO);
    } else if (read_write == I2C_SMBUS_READ) {
        if (size == I2C_SMBUS_BYTE_DATA) {
            pci_read_config_dword(priv->pci_dev, SMB_DATA_CFG(bus), &val);
            data->byte = val & SMB_RDATA_MASK_BYTE;
        } else if (size == I2C_SMBUS_WORD_DATA) {
            pci_read_config_dword(priv->pci_dev, SMB_DATA_CFG(bus), &val);
            data->word = (val & SMB_RDATA_MASK_WORD_MSB) >> 8 | (val & SMB_RDATA_MASK_WORD_LSB) << 8;
        }
        return (status & SMB_RDO ? 0 : -EIO);
    }
    return 0;
}

static const struct i2c_algorithm pcu_smb_smbus_algorithm = {
    .smbus_xfer       = pcu_smb_smbus_xfer,
    .functionality    = pcu_smb_func,
};

static int pcu_smb_init_bus(struct imc_priv *priv, int i)
{
    int err;

    i2c_set_adapdata(&priv->bus[i], priv);
    priv->bus[i].owner = THIS_MODULE;
    priv->bus[i].algo = &pcu_smb_smbus_algorithm;
    priv->bus[i].dev.parent = &priv->pci_dev->dev;

    snprintf(priv->bus[i].name, sizeof(priv->bus[i].name), "Skylake-X PCU adapter %d", i);
    err = i2c_add_adapter(&priv->bus[i]);
    if (err) {
        dev_info(&priv->pci_dev->dev, "name = %s err\n", priv->bus[i].name);
        return err;
    }

    return 0;
}

static void pcu_smb_free_bus(struct imc_priv *priv, int i)
{
    i2c_del_adapter(&priv->bus[i]);
}

static int pcu_smb_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
    int i, j, err;
    struct imc_priv *priv;

    priv = devm_kzalloc(&dev->dev, sizeof(*priv), GFP_KERNEL);
    if (!priv)
        return -ENOMEM;
    priv->pci_dev = dev;
    pci_set_drvdata(dev, priv);

    for (i = 0; i < NR_BUSES; i++) {
        err = pcu_smb_init_bus(priv, i);
        if (err) {
            dev_info(&dev->dev, "pcu_smb_init_bus err\n");
            goto probe_out_free_bus;
        }
    }

    return 0;

probe_out_free_bus:
    for (j = 0; j < i; j++)
        pcu_smb_free_bus(priv, j);
    return err;
}

static void pcu_smb_remove(struct pci_dev *dev)
{
    int i;
    struct imc_priv *priv = pci_get_drvdata(dev);

    for (i = 0; i < NR_BUSES; i++)
        pcu_smb_free_bus(priv, i);
}

static int pcu_smb_suspend(struct pci_dev *dev, pm_message_t mesg)
{
    struct imc_priv *priv = pci_get_drvdata(dev);
    priv->suspended = true;
    return 0;
}

static int pcu_smb_resume(struct pci_dev *dev)
{
    struct imc_priv *priv = pci_get_drvdata(dev);
    priv->suspended = false;
    return 0;
}

static const struct pci_device_id pcu_smb_ids[] = {
    { PCI_DEVICE(PCI_VENDOR_ID_INTEL, PCI_DEVICE_ID_INTEL_SKLX_PCU) },
    { 0, }
};
MODULE_DEVICE_TABLE(pci, pcu_smb_ids);

static struct pci_driver pcu_smb_pci_driver = {
    .name        = "pcu_smb",
    .id_table    = pcu_smb_ids,
    .probe       = pcu_smb_probe,
    .remove      = pcu_smb_remove,
    .suspend     = pcu_smb_suspend,
    .resume      = pcu_smb_resume,
};

static int __init pcu_smb_init(void)
{
    return pci_register_driver(&pcu_smb_pci_driver);
}
module_init(pcu_smb_init);

static void __exit pcu_smb_exit(void)
{
    pci_unregister_driver(&pcu_smb_pci_driver);
}
module_exit(pcu_smb_exit);

MODULE_AUTHOR("Florin9doi");
MODULE_DESCRIPTION("Intel Skylake-X PCU SMBus driver");
MODULE_LICENSE("GPL v2");
