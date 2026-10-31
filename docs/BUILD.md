# 固件构建与部署

## 环境和目标

- ESP-IDF：**5.4.4**；目标芯片：`esp32s3`。
- 板型：`CONFIG_BOARD_TYPE_MAG_AI_1688_MINI=y`。
- 模组：ESP32-S3-WROOM-1U-N16R8，16 MB Flash、8 MB Octal PSRAM。
- 本仓库基于上游历史快照；当前上游 README 或 IDF 6.x 的要求不能直接套用。

## 修改本机配置

在 `xiaozhi-esp32/main/application.cc` 顶部配置：

```cpp
#define WIFI_SSID     "你的WiFi名称"
#define WIFI_PASS     "你的WiFi密码"
#define SERVER_IP     "192.168.1.100"
#define SERVER_PORT   8888
#define LOCAL_PORT    8889
```

同时将 `TEACHER_TEXT` 中的示例电话改为实际心理老师联系方式。教师端应监听 `SERVER_PORT`，并使用同一校园局域网。请将真实网络凭据留在本地，提交前恢复占位值。

`ENABLE_CLOUD_FALLBACK` 当前默认是 `1`。若要全程使用局域网回应，将其改为 `0` 后重新编译；启用时可能向所配置的云服务发送语音。

## 编译

先激活 ESP-IDF 5.4.4 的命令行环境，再在 PowerShell 中执行：

```powershell
cd xiaozhi-esp32
Copy-Item -LiteralPath sdkconfig.mag-ai-1688-mini -Destination sdkconfig
idf.py build
```

`sdkconfig.mag-ai-1688-mini` 为现有 MAG AI 构建配置副本；`dependencies.lock` 记录依赖版本，首次构建需要下载对应组件。`main/application_xiaozhi.cc` 是上游存档，不参与应用编译。

## 固件包

Release 的固件 ZIP 具有以下结构：

```text
bootloader/bootloader.bin
partition_table/partition-table.bin
ota_data_initial.bin
srmodels/srmodels.bin
xiaozhi.bin
jieyou-treehole-mag-ai-1688-mini-merged.bin
flash_args
flasher_args.json
SHA256SUMS.txt
README.md
```

| 地址 | 文件 |
| --- | --- |
| `0x0` | `bootloader/bootloader.bin` |
| `0x8000` | `partition_table/partition-table.bin` |
| `0xd000` | `ota_data_initial.bin` |
| `0x10000` | `srmodels/srmodels.bin` |
| `0x200000` | `xiaozhi.bin` |

Flash 参数：`dio`、`80m`、`16MB`。合并固件从 `0x0` 开始，不能把单独的 `xiaozhi.bin` 当作从 `0x0` 写入的完整镜像。

预编译包内仍是示例网络设置。配置和重新编译完成后，人工核对板型、供电和串口，再使用自己的构建产物烧录。以下操作会写入硬件，应由设备操作者确认后执行：

```powershell
idf.py -p COMxx flash
```

只查看已烧录设备的串口日志时，使用 `idf.py -p COMxx monitor`。本次归档发布没有执行烧录或真机测试。
