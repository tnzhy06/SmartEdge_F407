/**
 * @file    bsp_flash.c
 * @brief   STM32F407 片内 Flash Sector 11 安全擦写板级驱动实现
 * @date    2026-11-27
 * @note    擦除 128 KB 扇区期间 Flash 总线挂起，采用 32-bit Word 编程并逐字节 memcmp 校验
 */

#include "bsp_flash.h"
#include "stm32f4xx_hal.h"
#include <stddef.h>
#include <string.h>

AppErr_t Bsp_FlashRead(uint32_t addr, void *buf, uint32_t len)
{
  if ((buf == NULL) || (len == 0U))
  {
    return APP_ERR_PARAM;
  }
  if ((addr < BSP_FLASH_BASE_ADDR) || (addr >= BSP_FLASH_END_ADDR) ||
      (len > (BSP_FLASH_END_ADDR - addr)))
  {
    return APP_ERR_PARAM;
  }

  memcpy(buf, (const void *)addr, len);
  return APP_OK;
}

AppErr_t Bsp_FlashEraseWriteSector11(const void *data, uint32_t len_bytes)
{
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t sector_err = 0UL;
  const uint8_t *src = (const uint8_t *)data;
  HAL_StatusTypeDef hal_st;
  AppErr_t ret = APP_OK;

  if ((data == NULL) || (len_bytes == 0U) || (len_bytes > BSP_FLASH_SECTOR11_SIZE) ||
      ((len_bytes & 0x3UL) != 0UL))
  {
    return APP_ERR_PARAM;
  }
  if (__get_IPSR() != 0U)
  {
    return APP_ERR_PERM;
  }

  /* M5 启用 IWDG 后须在此处（擦除前）及编程后各执行一次 Bsp_WdtRefresh() 防止 1-2 s 擦除触发复位 */

  if (HAL_FLASH_Unlock() != HAL_OK)
  {
    return APP_ERR_IO;
  }

  /* 清除可能残留的历史错误标志，否则后续擦除/编程会被硬件拒绝 */
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_OPERR | FLASH_FLAG_WRPERR |
                         FLASH_FLAG_PGAERR | FLASH_FLAG_PGPERR | FLASH_FLAG_PGSERR);

  erase.TypeErase = FLASH_TYPEERASE_SECTORS;
  erase.VoltageRange = FLASH_VOLTAGE_RANGE_3; /* 2.7~3.6 V 供电支持 32-bit 并行度 */
  erase.Sector = FLASH_SECTOR_11;
  erase.NbSectors = 1UL;

  hal_st = HAL_FLASHEx_Erase(&erase, &sector_err);
  if ((hal_st != HAL_OK) || (sector_err != 0xFFFFFFFFUL))
  {
    ret = APP_ERR_IO;
  }

  if (ret == APP_OK)
  {
    for (uint32_t offset = 0UL; offset < len_bytes; offset += 4UL)
    {
      uint32_t word = 0UL;

      /* 使用 memcpy 装入 32 位字，避免调用方指针非 4 字节对齐引发未定义行为 */
      memcpy(&word, src + offset, sizeof(word));
      hal_st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                                 BSP_FLASH_SECTOR11_ADDR + offset,
                                 (uint64_t)word);
      if (hal_st != HAL_OK)
      {
        ret = APP_ERR_IO;
        break;
      }
    }
  }

  (void)HAL_FLASH_Lock();

  /* 写入结束后逐字节回读比对，确认 Flash 内容与源缓冲 100% 一致 */
  if (ret == APP_OK)
  {
    if (memcmp((const void *)BSP_FLASH_SECTOR11_ADDR, data, len_bytes) != 0)
    {
      ret = APP_ERR_IO;
    }
  }

  return ret;
}
