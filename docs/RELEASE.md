# 发布清单与验证范围

发布标签：`firmware-2026.10.31`，预发布归档。

## 构建来源

- 归档现有 MAG AI 构建（本地文件时间 2026-09-15），ESP-IDF 5.4.4、ESP32-S3。
- 本次未重新编译、未连接硬件、未烧录；不宣称真机五场景通过。
- 现有应用镜像通过 esptool 4.12.0 的镜像校验和与 SHA-256 验证。
- 合并镜像按现有 flasher_args.json 生成；每个分区在合并结果的对应地址逐字节核对。
- 源码副本与工作区固件文件逐个核对 SHA-256；保留上游许可、板型配置和依赖锁。
- 预编译包保留示例配置，实际部署须修改 Wi-Fi、教师端 IP 和联系方式后重新编译。

## 工作区回归

2026-10-31 在原项目工作区运行 `python tests/run_all.py`：**126 项 / PASS 126 / FAIL 0 / SKIP 0**，其中行为检查 49 项、静态源码检查 77 项。该结果涵盖电脑端通信与音频等检查，不等同于固件真机验收；原项目测试与教师端本次不随固件源码包发布。

## 二进制文件

| 文件 | 烧录地址 | 字节数 | SHA-256 |
| --- | --- | ---: | --- |
| `bootloader/bootloader.bin` | `0x0` | 16432 | `9acfb56f4da924f0b5cab120a9649b5f12da388f7a523dc614faf148312c5ac1` |
| `xiaozhi.bin` | `0x200000` | 2588176 | `928f8f6febaf785fe3a8ec5c4ad814c86bcd418d9e46bc198097a8c43a2bbdaf` |
| `partition_table/partition-table.bin` | `0x8000` | 3072 | `ec035284948c79f712f9a37279aba62794d85e722c854866b7b6a1f678d311c9` |
| `ota_data_initial.bin` | `0xd000` | 8192 | `7d2c7ac4888bfd75cd5f56e8d61f69595121183afc81556c876732fd3782c62f` |
| `srmodels/srmodels.bin` | `0x10000` | 291042 | `7c87dd7adb5a7623907b6354d49bea7f9289371262e6a57f15458fb8908c5814` |
| `jieyou-treehole-mag-ai-1688-mini-merged.bin` | `0x0` | 4685328 | `89d61ff12b195691b43ed28361f93a82638e2c6e5ce2129394b98da65f6b60de` |

## 发布边界

源码包包含固件、构建说明、项目介绍和用户指定的项目负责人联系图片；
不包含申报书原件、校园录音、教师端本机配置、上游 Git 历史、缓存、ELF 或调试日志。
教师端与 OpenDesign 网页界面的后续集成不作为本固件归档的验收结论。

`SOURCE_MANIFEST.json` 记录源码包内文件的 SHA-256（清单本身除外）。
Release 中的 `SHA256SUMS.txt` 记录两个下载压缩包的 SHA-256。
