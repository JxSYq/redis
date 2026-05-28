# Redis 命令时延统计扩展 — 需求设计文档

## 一、概述

### 1.1 背景

Redis 原生 `INFO Commandstats` 仅提供 `calls`、`usec`、`usec_per_call`、`rejected_calls`、`failed_calls` 等基础统计。在生产环境中，运维和开发人员需要更丰富的延迟分布信息（如 P95/P99）以及近期时间窗口内的趋势数据，以便快速定位性能抖动。

### 1.2 目标

在 `INFO Commandstats` 中为每个命令扩展以下维度的延迟统计：

| 时间范围 | 新增指标 |
|---------|---------|
| 全量（启动至今） | `usec_min`、`usec_max`、`usec_p95`、`usec_p99` |
| 最近 5 秒 | `calls_5s`、`usec_5s`、`usec_min_5s`、`usec_avg_5s`、`usec_max_5s` |
| 最近 1 分钟 | `calls_minute`、`usec_minute`、`usec_min_minute`、`usec_max_minute`、`usec_avg_minute`、`usec_p95_minute`、`usec_p99_minute` |

### 1.3 设计原则

- **默认关闭**：通过配置项显式开启，不影响未启用场景的性能
- **单线程安全**：所有操作在 Redis 主线程中完成，无并发风险

---

## 二、配置项设计

### 2.1 配置项清单

```bash
# redis.conf

# 是否开启命令延迟扩展统计
# yes: 开启  no: 关闭（默认）
command-latency-tracking no

### 2.2 运行时修改

```bash
# 开启 / 关闭
CONFIG SET command-latency-tracking yes
CONFIG SET command-latency-tracking no
---

## 三、输出格式

### 3.1 功能关闭时（默认，与原版完全一致）

```
# Commandstats
cmdstat_get:calls=21638,usec=1706779,usec_per_call=78,rejected_calls=0,failed_calls=0
cmdstat_set:calls=21648,usec=1695579,usec_per_call=78,rejected_calls=0,failed_calls=0
```

### 3.2 功能开启时

```
# Commandstats
cmdstat_-:calls=43292,usec=3403840,usec_per_call=78,rejected_calls=0,failed_calls=0,usec_min=8,usec_max=12871,usec_p95=175,usec_p99=275,calls_5s=0,usec_5s=0,usec_min_5s=0,usec_avg_5s=0,usec_max_5s=0,calls_minute=4,usec_minute=934,usec_min_minute=76,usec_avg_minute=233,usec_max_minute=329,usec_p95_minute=329,usec_p99_minute=329
cmdstat_o:calls=6,usec=1482,usec_per_call=247,rejected_calls=0,failed_calls=0,usec_min=87,usec_max=329,usec_p95=329,usec_p99=329,calls_5s=0,usec_5s=0,usec_min_5s=0,usec_avg_5s=0,usec_max_5s=0,calls_minute=3,usec_minute=858,usec_min_minute=221,usec_avg_minute=286,usec_max_minute=329,usec_p95_minute=329,usec_p99_minute=329
cmdstat_r:calls=21638,usec=1706779,usec_per_call=78,rejected_calls=0,failed_calls=0,usec_min=8,usec_max=12871,usec_p95=175,usec_p99=274,calls_5s=0,usec_5s=0,usec_min_5s=0,usec_avg_5s=0,usec_max_5s=0,calls_minute=0,usec_minute=0,usec_min_minute=0,usec_avg_minute=0,usec_max_minute=0,usec_p95_minute=0,usec_p99_minute=0
cmdstat_w:calls=21648,usec=1695579,usec_per_call=78,rejected_calls=0,failed_calls=0,usec_min=8,usec_max=4087,usec_p95=175,usec_p99=275,calls_5s=0,usec_5s=0,usec_min_5s=0,usec_avg_5s=0,usec_max_5s=0,calls_minute=0,usec_minute=0,usec_min_minute=0,usec_avg_minute=0,usec_max_minute=0,usec_p95_minute=0,usec_p99_minute=0
cmdstat_command:calls=5,usec=1395,usec_per_call=279,rejected_calls=0,failed_calls=0,usec_min=221,usec_max=329,usec_p95=329,usec_p99=329,calls_5s=0,usec_5s=0,usec_min_5s=0,usec_avg_5s=0,usec_max_5s=0,calls_minute=3,usec_minute=858,usec_min_minute=221,usec_avg_minute=286,usec_max_minute=329,usec_p95_minute=329,usec_p99_minute=329
cmdstat_get:calls=21638,usec=1706779,usec_per_call=78,rejected_calls=0,failed_calls=0,usec_min=8,usec_max=12871,usec_p95=175,usec_p99=274,calls_5s=0,usec_5s=0,usec_min_5s=0,usec_avg_5s=0,usec_max_5s=0,calls_minute=0,usec_minute=0,usec_min_minute=0,usec_avg_minute=0,usec_max_minute=0,usec_p95_minute=0,usec_p99_minute=0
cmdstat_set:calls=21648,usec=1695579,usec_per_call=78,rejected_calls=0,failed_calls=0,usec_min=8,usec_max=4087,usec_p95=175,usec_p99=275,calls_5s=0,usec_5s=0,usec_min_5s=0,usec_avg_5s=0,usec_max_5s=0,calls_minute=1,usec_minute=76,usec_min_minute=76,usec_avg_minute=76,usec_max_minute=76,usec_p95_minute=76,usec_p99_minute=76
```

### 3.3 聚合分类行

除每个具体命令外，额外输出 4 个聚合分类行：

| 分类 | 含义 | 分类依据 |
|------|------|---------|
| `cmdstat_-` | 所有命令聚合 | 全部命令 |
| `cmdstat_r` | 读命令聚合 | `cmd->flags & CMD_READONLY` |
| `cmdstat_w` | 写命令聚合 | 非 READONLY 且非管理命令 |
| `cmdstat_o` | 其他命令聚合 | `PING`、`INFO`、`CONFIG`、`COMMAND` 等管理类 |

每个命令执行时同步更新自身统计 + 对应分类统计 + `cmdstat_-` 聚合统计，共 3 个统计对象。
info 先输出聚合行，再输出各命令的统计行

### 3.4 各指标定义

#### 3.4.1 全量指标（自启动或上次 RESETSTAT 以来）

| 字段 | 类型 | 含义 |
|------|------|------|
| `usec_min` | uint64 | 最小执行耗时（微秒），无调用时为 0 |
| `usec_max` | uint64 | 最大执行耗时（微秒），无调用时为 0 |
| `usec_p95` | uint64 | 第 95 百分位耗时（微秒），精度取决于直方图桶边界 |
| `usec_p99` | uint64 | 第 99 百分位耗时（微秒） |

#### 3.4.2 5 秒窗口指标

| 字段 | 类型 | 含义 |
|------|------|------|
| `calls_5s` | uint64 | 最近 5 秒调用次数 |
| `usec_5s` | uint64 | 最近 5 秒总耗时 |
| `usec_min_5s` | uint64 | 最近 5 秒最小耗时，无调用时为 0 |
| `usec_avg_5s` | uint64 | 最近 5 秒平均耗时（`usec_5s / calls_5s`），无调用时为 0 |
| `usec_max_5s` | uint64 | 最近 5 秒最大耗时，无调用时为 0 |

#### 3.4.3 1 分钟窗口指标

| 字段 | 类型 | 含义 |
|------|------|------|
| `calls_minute` | uint64 | 最近 1 分钟调用次数 |
| `usec_minute` | uint64 | 最近 1 分钟总耗时 |
| `usec_min_minute` | uint64 | 最近 1 分钟最小耗时，无调用时为 0 |
| `usec_avg_minute` | uint64 | 最近 1 分钟平均耗时，无调用时为 0 |
| `usec_max_minute` | uint64 | 最近 1 分钟最大耗时，无调用时为 0 |
| `usec_p95_minute` | uint64 | 最近 1 分钟第 95 百分位耗时，无调用时为 0 |
| `usec_p99_minute` | uint64 | 最近 1 分钟第 99 百分位耗时，无调用时为 0 |

---

### 补充要求：
对于带有子命令的命令，开启该功能时，子命令被合并到父命令统计中；
不开启该功能时，输出格式不变