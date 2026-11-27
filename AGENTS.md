# AGENTS.md — SmartEdge_F407 AI 辅助开发规范与工程约束

本文件面向辅助开发 **SmartEdge_F407**（基于立创天空星 `STM32F407VGT6` + `FreeRTOS`）的 AI Agent 与开发者，定义本项目必须遵守的工程边界、编码与中文 Doxygen 注释规范、嵌入式硬件/并发红线、Git/GitHub 版本管理规范以及提交前独立代码评审门禁。

---

## 1. 项目定位与文档真源（Source of Truth）

- **项目性质**：大学生嵌入式综合学习与实战开发项目。代码应**清晰、直白、易单步调试、易维护**，**严禁过度设计**（不引入未经确认的复杂框架、冗余抽象层或外部库）。
- **工程根目录**：`D:\SmartEdge_F407` 直接作为 STM32CubeMX 原生 CMake 工程根目录。
- **四份基线文档（动手写代码前必读，严禁擅自违背）**：
  1. [软件需求规格说明书（SRS）](./docs/SmartEdge_F407_软件需求规格说明书_SRS.md)（功能边界、协议基线、验收项 `AT-01`~`AT-10`）
  2. [硬件资源分配表（HW）](./docs/SmartEdge_F407_硬件资源分配表.md)（82 引脚分配、时钟树、DMA、NVIC、接线与电源）
  3. [软件架构设计文档（SAD）](./docs/SmartEdge_F407_软件架构设计文档_SAD.md)（CubeMX 目录风格、任务与内存预算、技术选型）
  4. [软件详细设计文档（SDD）](./docs/SmartEdge_F407_软件详细设计文档_SDD.md)（模块 API、数据结构、状态机、9 大防坑评审与 `M1–M5` 联调路线）
- **文档与代码同步铁律**：若开发中确需修改引脚、寄存器地址、CAN 帧结构、故障位掩码或任务栈配置，必须**先向用户说明原因并同步更新对应基线文档**，不允许代码与文档长期脱节。

---

## 2. 目录结构与 STM32CubeMX 保护边界

- **CubeMX 自动生成区（高危保护区）**：
  - `Core/`、`Drivers/`、`FATFS/`、`LWIP/`、`Middlewares/Third_Party/(FreeRTOS,LwIP,FatFs)`、`cmake/stm32cubemx/`、`STM32F407xx_FLASH.ld` 均由 STM32CubeMX 自动维护。
  - **严禁**在 CubeMX 生成文件的 `USER CODE` 注释块之外添加代码；所有胶水调用必须严格放在 `/* USER CODE BEGIN ... */` 与 `/* USER CODE END ... */` 之间。
  - **严禁**手改 `STM32F407xx_FLASH.ld`、`cmake/stm32cubemx/CMakeLists.txt` 与 `SmartEdge_F407.ioc`（`.ioc` 由开发者按 Agent 提供的配置指南在 STM32CubeMX 图形界面亲自配置并生成）。
- **自研代码区（延续 CubeMX `Inc/` + `Src/` 风格）**：
  - `BSP/Inc/`、`BSP/Src/`：板级外设驱动（仅允许依赖 HAL 与 CubeMX 生成头文件，**严禁反向依赖 `App/`**）。
  - `App/Inc/`、`App/Src/`：业务任务、全局状态 `g_sys`、轻量 Modbus/CAN 协议、LVGL UI 页面、配置存储及 OTA/低功耗接口桩。
  - `Middlewares/Third_Party/lvgl/`：LVGL v9 第三方图形库源码与 `lv_conf.h`。
  - 根 `CMakeLists.txt` 仅在用户追加区加入 `BSP/Src/*.c`、`App/Src/*.c` 与 LVGL 源文件及头文件路径。

---

## 3. 基本开发与 C11 编码规范

### 3.1 格式与命名
- **语言与编码**：纯 **C11**；文件统一 **UTF-8（无 BOM）**，行尾 **LF**，与 STM32CubeMX 保持一致采用 **2 空格缩进 + Allman 大括号换行风格**（禁用 Tab，由根目录 `.clang-format` 统一约束），单行建议 `≤ 100` 字符。
- **文件命名**：全小写下划线，带明确层级前缀：
  - BSP 驱动：`bsp_<外设>.h/.c`（如 `bsp_rs485.c`、`bsp_st7789.c`）。
  - 业务/协议/UI：`app_<模块>.h/.c`（如 `app_control.c`、`app_modbus_tcp.c`）、`ui_<页面>.c`（如 `ui_page_overview.c`）。
- **标识符命名**：
  - 公开函数：`模块前缀_大驼峰`，如 `Bsp_Rs485Transceive()`、`AppCtrl_SetThresholds()`、`ModbusRtu_Crc16()`。
  - 文件内部函数与变量：必须加 `static` 限制作用域；静态变量加 `s_` 前缀，跨文件全局变量加 `g_` 前缀。
  - 宏与枚举常量：全大写下划线（`APP_CFG_MAGIC`、`FAULT_CAN_STALL`）。
  - 自定义结构体/枚举类型：大驼峰加 `_t` 后缀（`AppState_t`、`AppErr_t`）。

### 3.2 防御性编码与安全约定
- **定宽类型**：协议帧、寄存器、硬件配置与共享状态一律使用 `<stdint.h>` 定宽整型（`uint8_t`、`int16_t`、`uint32_t`），禁止用裸 `int`/`long` 表达有明确位宽的数据。
- **统一返回码**：模块接口统一返回 `AppErr_t`（`APP_OK = 0`，失败为负值），调用方必须检查关键错误码，禁止静默吞错。
- **指针与缓冲区安全**：公开 API 入口必须判空；字符串与内存拷贝强制使用 `snprintf`、`strncpy`（补 `\0`）、`memcpy` 并校验上限，**严禁使用 `sprintf`、`strcpy`、`gets`**。
- **静态断言与头保护**：所有 `.h` 必须有 `#ifndef XXX_H` 守卫；跨介质存储结构体（如 `AppConfig_t`、`CanFrame_t`）必须附 `_Static_assert(sizeof(...) == ...)`。
- **纯英文 UI 约束**：LVGL 界面统一纯英文，仅启用内置 `Montserrat` 字体（`12/14/16/24`），不引入任何外部/中文字库；温度单位使用 `"°C"`（`U+00B0`）或 `"C"`，**禁止使用 CJK 字符 `℃`（`U+2103`）**。
- **零警告标准**：自研代码在 `-Wall -Wextra` 下应做到 **0 Error、0 Warning**，禁止遗留未使用的变量/函数或无解释的强制类型转换。

---

## 4. Doxygen 风格中文注释规范（强制要求）

所有自研 `.h` 和 `.c` 文件**必须**编写规范的 **Doxygen 风格中文注释**，并在关键逻辑处补充**行内中文注释**。

### 4.1 必须标注注释的位置
1. **文件头注释**：每个 `.h` 和 `.c` 顶部必须写明 `@file`、`@brief`、`@date`、`@note`（说明硬件依赖或模块职责）。
2. **所有公开函数（`.h` 中）与核心静态函数（`.c` 中）**：必须写明 `@brief`、`@param[in]`/`@param[out]`/`@param[in,out]`、`@return`；涉及中断安全、阻塞等待、持锁约束或硬件副作用时，必须补充 `@note` 或 `@warning`。
3. **结构体、枚举与关键宏**：顶部写 `/** @brief ... */`，字段与枚举项行尾使用 `/**< 中文说明（含单位/范围） */`。
4. **关键语句中文注释**：硬件寄存器/外设时序切换、状态机条件跃迁、协议字节拼装/校验、并发加锁/信号唤醒、魔数与边界保护处必须写明**设计意图（Why）**，而非机械翻译语法。

### 4.2 标准注释范例

```c
/**
 * @file    bsp_rs485.h
 * @brief   RS-485 半双工通信板级驱动（基于 USART2 DMA + PD4 方向控制）
 * @date    2026-11-26
 * @note    依赖 CubeMX 生成的 huart2、DMA1 Stream5/6 与 SensorTaskHandle
 */

#ifndef BSP_RS485_H
#define BSP_RS485_H

#include <stdint.h>
#include "app_err.h"

/**
 * @brief  执行一次 RS-485 主站半双工收发事务
 * @param[in]  tx          待发送数据缓冲区指针（非空）
 * @param[in]  tx_len      发送字节数（1..64）
 * @param[out] rx          接收数据缓冲区指针（非空）
 * @param[in]  rx_max      接收缓冲区最大容量
 * @param[out] rx_len      实际接收到的字节数
 * @param[in]  timeout_ms  等待从站应答超时时间（单位：ms）
 * @return AppErr_t        APP_OK 成功；APP_ERR_TIMEOUT 超时；APP_ERR_PARAM 参数非法
 * @warning 本函数会阻塞等待线程标志，禁止在中断上下文调用，禁止持 sysMutex 时调用
 */
AppErr_t Bsp_Rs485Transceive(const uint8_t *tx, uint16_t tx_len,
                             uint8_t *rx, uint16_t rx_max,
                             uint16_t *rx_len, uint32_t timeout_ms);

#endif /* BSP_RS485_H */
```

行内关键语句注释示例：
```c
/* 关闭 DMA 半传输中断，防止 9 字节 KTH2-R 应答在收到一半时被提前截断 */
__HAL_DMA_DISABLE_IT(huart2.hdmarx, DMA_IT_HT);

/* 必须在 TC（发送完全结束）中断中才拉低 PD4，避免最后一字节停止位被截断 */
HAL_GPIO_WritePin(RS485_DE_GPIO_Port, RS485_DE_Pin, GPIO_PIN_RESET);
```

---

## 5. STM32F407 & FreeRTOS 不可违反的工程红线

Agent 编写或修改任何驱动/任务代码时，必须逐条守住以下硬件与 RTOS 红线（详见 `SDD` 第 7.1 节）：

1. **HAL 时基**：必须使用 `TIM6`，严禁使用 `SysTick`。
2. **CCMRAM 禁 DMA**：仅 `g_lvgl_mem[48*1024]` 和 `g_trend` 放入 `.ccmram`；所有任务栈与 DMA/FatFs/LwIP/ETH/SPI/UART 缓冲必须位于默认 128 KB 主 RAM。
3. **单锁短持与 ISR 安全**：全局仅 `sysMutex` 一把锁，持锁时间 `< 20 µs`，严禁持锁做任何阻塞 IO；ISR 中严禁调用 `osMutexAcquire` 或阻塞 API。
4. **LVGL 单线程**：仅 `HmiTask` 可调用 `lv_*` 函数，其他任务严禁直接操作 UI 控件。
5. **UART DMA IDLE 防半传输坑**：每次调用 `HAL_UARTEx_ReceiveToIdle_DMA()` 后必须关闭 `DMA_IT_HT`。
6. **RS-485 方向切换**：必须在 `HAL_UART_TxCpltCallback`（`TC`）中拉低 `PD4`，禁止在 `TXE` 时切换。
7. **ETH 与板载选贴 SPI Flash 隔离**：在 `MX_ETH_Init()` 前先将 `PA4` 推挽拉高；LAN8720 PHY 地址为 `1`，初始化通过 MDIO 软复位且**不得死等网线插入**。
8. **CAN2 依赖**：必须先开启 `CAN1` 时钟并在 `CAN1` 上配置 `SlaveStartFilterBank = 14`；发送前检查空闲邮箱防卡死。
9. **SDIO 与 FatFs**：关闭 STM32F407 SDIO 硬件流控（规避硅片勘误），`SDIO_IRQn (5)` 优先级必须严格高于 `DMA2_Stream3/6_IRQn (6)`；`LogTask` 不纳入 IWDG 硬复位监督。
10. **Sector 11 擦写保护**：擦写 128 KB `Sector 11` 前后必须双次喂狗 `Bsp_WdtRefresh()`，写完后重置 CAN 心跳计时基准，防止 1–2 s 取指暂停触发误复位或误报 CAN 失联。

---

## 6. Git 与 GitHub 版本管理规范

本项目使用 **Git** 进行本地版本控制，并托管在 **GitHub** 远程仓库。

### 6.1 分支模型与命名
- **`main`**：唯一稳定主分支，任何时候都应保持可编译、与文档一致；**禁止直接向 `main` 提交未经评审的代码，严禁对 `main` 执行 `git push --force`**。
- **开发分支（从 `main` 检出，按模块或里程碑 `M1–M5` 拆分，小步提交）**：
  - `feat/<阶段或模块>`：新功能开发（如 `feat/m1-cli-monitor`、`feat/m2-hmi-lvgl`、`feat/m3-rs485-can`）。
  - `fix/<问题描述>`：缺陷修复（如 `fix/rs485-dma-ht-irq`、`fix/modbus-batch-threshold`）。
  - `docs/<主题>`：文档更新（如 `docs/baseline-specs`、`docs/update-sdd`）。
  - `refactor/<模块>`：不改变外部行为的代码重构。
  - `chore/<事项>`：构建脚本、`.gitignore`、工具配置维护。

### 6.2 Commit 提交信息规范（Conventional Commits + 中文说明）
- 格式：`<type>(<scope>): <简明中文摘要>`（不超过 50 字，必要时在空行后补充改动背景与验证方式）。
- 允许的 `<type>`：`feat`、`fix`、`docs`、`refactor`、`test`、`build`、`chore`。
- 示例：
  - `feat(bsp_rs485): 实现 USART2 DMA 与 TC 方向切换驱动`
  - `fix(app_modbus_tcp): 修复 0x10 批量写温控阈值误拦截缺陷`
  - `docs(specs): 定稿 SRS/HW/SAD/SDD 四份基线文档`
- **原子提交与仓库洁净**：
  - 每个 Commit 仅包含单一清晰主题；禁止把不相关改动打包进同一个 Commit。
  - 必须维护 `.gitignore`，**严禁提交** `build/`、CMake 缓存、编译产物（`*.elf`/`*.bin`/`*.hex`/`*.map`/`*.o`）、IDE 本地配置（如 `.vscode/` 个人路径）以及任何私密信息/令牌。

### 6.3 合并（Merge / GitHub PR）规范
- 合并至 `main` 前必须满足：
  1. 已同步最新 `main` 并无冲突（推荐 `git fetch origin && git rebase origin/main`）；
  2. 本地构建通过（0 Error，自研代码 0 Warning）；
  3. 已完成第 7 节规定的**独立对话代码评审**且无未关闭的 `Blocker`/`Major` 问题。
- **GitHub 合并方式**：
  - 里程碑级功能分支（如 `feat/m1-*` ~ `feat/m5-*`）通过 GitHub Pull Request 使用 **Create a merge commit (`--no-ff`)** 合入 `main`，保留清晰的里程碑脉络；
  - 零碎小修复分支可使用 **Squash and merge** 保持 `main` 提交树整洁；
  - 合并完成后及时删除已合入的本地与远端临时分支。

### 6.4 版本号与标签（SemVer & Tag）规范
- 采用语义化版本 **`vMAJOR.MINOR.PATCH`**（遵循 [SemVer 2.0.0](https://semver.org/)）：
  - 开发里程碑预发布标签：`v0.1.0-m1`（M1 最小系统+CLI）、`v0.2.0-m2`（M2 显示+触控）、`v0.3.0-m3`（M3 485+双 CAN 闭环）、`v0.4.0-m4`（M4 以太网+Modbus TCP+SNTP）、`v0.5.0-m5`（M5 TF 日志+配置+IWDG）。
  - 全部 `AT-01`~`AT-10` 验收通过后发布正式基线 **`v1.0.0`**。
  - 后续向后兼容的新增功能递增 `MINOR`（`v1.1.0`），缺陷修复递增 `PATCH`（`v1.0.1`）。
- 统一使用**附注标签（Annotated Tag）**并同步到 GitHub Releases：
  ```bash
  git tag -a v0.1.0-m1 -m "M1: 完成 CubeMX 工程骨架、USART1 CLI 与复位原因检测"
  git push origin v0.1.0-m1
  ```

### 6.5 Agent 执行 Git 命令的安全红线
- Agent 可随时运行只读命令（`git status`、`git diff`、`git log`、`git branch`）。
- **未经用户明确授权，Agent 严禁自行执行**：`git commit`、`git push`、`git tag`、`git reset --hard`、`git checkout -- <file>`、`git clean -fd` 或任何改写远端历史的操作。

---

## 7. 提交前独立对话代码评审门禁（强制流程）

为避免“同一对话内自写自审”的上下文盲区，**每次执行 `git commit`（及合并至 `main`）之前，必须单开一个全新的独立对话进行代码评审**：

### 7.1 标准执行步骤
1. **开发对话**：完成当前小任务或里程碑编码与编译验证后，保持改动在工作区/暂存区，向用户汇报改动清单并**停止，不直接提交**。
2. **新开独立评审对话**：用户另开一个干净的新对话，让评审 Agent 仅依据 `git diff`、`AGENTS.md` 与 `docs/` 基线文档执行独立审查（参考提示词：“*请作为独立代码评审员，阅读 AGENTS.md 与 docs/ 下对应设计文档，审查当前工作区 `git diff`，按 Blocker / Major / Minor 列出全部问题。*”）。
3. **评审必查清单**：
   - [ ] **规格与接口一致**：引脚、寄存器地址、CAN ID/字节序、故障位、返回码是否严格符合 `SRS`/`HW`/`SDD`；
   - [ ] **第 5 节 10 条嵌入式红线**：是否存在 CCMRAM DMA、ISR 调 Mutex、持锁做 IO、忘关 `DMA_IT_HT`、`TXE` 切 RS-485 方向、LVGL 跨线程调用、Sector 11 擦写未双喂狗等致命隐患；
   - [ ] **CubeMX 保护边界**：CubeMX 生成文件内的改动是否全部严格位于 `USER CODE` 块内；
   - [ ] **注释规范**：是否具备完整的 Doxygen 风格中文注释与关键语句中文注释；
   - [ ] **边界与健壮性**：空指针、数组越界、整数溢出、负温度补码、TCP 半包/粘包、超时与异常恢复分支是否完整；
   - [ ] **仓库洁净**：是否夹带编译产物、调试垃圾代码或无关文件修改。
4. **修复与放行**：
   - 存在任何 **`[Blocker 阻断]`** 或 **`[Major 重要]`** 问题时，**禁止提交**，须修复后复审通过；
   - 评审无阻断项且用户确认后，方可执行 `git commit` 与推送 GitHub。

---

## 8. 学习型项目分阶段协作流程与 Agent 行为准则（强制）

本项目以**掌握 STM32F407 + FreeRTOS 工程实战能力**为核心学习目标，严格遵循 `SDD` 第 7.3 节的 **`M1–M5` 五阶段路线**。每个阶段必须严格执行以下四步闭环，**严禁跳步或擅自推进**：

### 8.1 每阶段标准四步闭环

1. **步骤一：CubeMX 手把手配置指导（用户亲手操作）**
   - 每个阶段开始前，Agent 必须先提供**详细的 STM32CubeMX 图形界面配置指南**，逐项写明：
     - 需开启的外设及具体菜单路径；
     - 引脚分配、复用模式、上下拉与输出速度；
     - 参数配置（时钟分频、波特率、滤波器、PWM 周期、FreeRTOS 任务名/优先级/`Words` 栈大小/队列与互斥锁等）；
     - DMA 数据流/通道/方向与 NVIC 抢占优先级；
     - 关键选项背后的**硬件/RTOS 原理（Why）**及易错勾选提醒。
   - 随后**停止等待**，由开发者亲自在 STM32CubeMX 中配置并点击生成代码，确认完成后通知 Agent。Agent **严禁**绕过用户直接篡改 `.ioc` 或伪造 CubeMX 生成文件。
2. **步骤二：当前阶段编码与构建验证（Agent 实施）**
   - 确认用户已生成本阶段 CubeMX 代码后，Agent 先检查新生成的 `Core/`、`FATFS/`、`LWIP/` 等上下文，仅编写**当前阶段范围内**的 `BSP/`、`App/` 与 `USER CODE` 胶水代码。
   - 若本机已安装 ARM GCC / CMake，须实际执行编译验证（0 Error，自研代码 0 Warning）；若暂无交叉编译环境，必须如实说明“未执行交叉编译，仅完成静态检查”，绝不虚报结果。
3. **步骤三：强制代码讲解与上板验收指引（严禁“只写不说”）**
   - 每阶段代码编写完成后，Agent **必须**给出结构化的代码讲解，帮助开发者真正理解每一块核心代码，讲解内容至少包含：
     - **文件与调用链路**：本阶段新增/修改了哪些文件，任务、中断回调与驱动之间如何串联运转；
     - **核心逻辑拆解**：关键状态机、协议帧解析、DMA/中断协作、互斥锁/信号同步的具体代码走读；
     - **防坑设计意图（Why）**：解释关键语句为什么这样写（对应第 5 节的硬件或 RTOS 原理）；
     - **烧录与自测指引**：给出本阶段清晰的验证步骤、串口命令/测试动作、预期现象及常见故障排查点。
4. **步骤四：独立评审、消化吸收与阶段门禁（强制停顿）**
   - 开发者消化讲解内容、完成实测验证并按第 7 节完成**独立对话代码评审**及提交后，**只有收到开发者的明确指令（如“开始 M2 阶段”）**，Agent 才可进入下一阶段。
   - **红线禁止**：未经用户明确放行，严禁提前编写下一阶段的代码或顺手把后续阶段的功能一起实现。

### 8.2 日常协作补充原则
- **先查后改、实事求是**：修改任何文件前先读取实际内容；发现 CubeMX 生成结果与预期不符时，先向开发者指出差异并说明如何调整配置，不偷偷打补丁掩盖。
- **主动提示硬件风险**：凡涉及引脚接线、电源配置（如触摸屏 `J1`、KTH2-R `8–28 V` 共地）、Flash 擦写或看门狗启停时，必须主动发出风险提醒。
- **随时答疑**：在任意阶段，优先解答开发者关于代码细节、CubeMX 选项、HAL/FreeRTOS 底层机制的提问，确保“写懂每一阶段，再进下一阶段”。
