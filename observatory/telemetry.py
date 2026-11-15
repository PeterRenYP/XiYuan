"""独立 NVML 进程；模型暂停或崩溃时整卡监控仍工作。"""
from __future__ import annotations

import datetime as dt
import os
import sys
import time

from .protocol import Emitter


def payload(status, reason=None, **values):
    """同时提供显示字段与统一指标契约，缺失指标保留状态和原因。"""
    timestamp = dt.datetime.now(dt.timezone.utc).isoformat()
    metrics = {}
    for name, field, unit in (("memory_used", "used", "bytes"), ("memory_total", "total", "bytes"), ("gpu_utilization", "utilization", "percent")):
        metrics[name] = {"value": values.get(field), "status": status, "source": "NVML", "unit": unit, "sample_time": timestamp, "reason": reason}
    return {"status": status, "reason": reason, "source": "NVML", "device_uuid": values.get("uuid", "unavailable"),
            "runtime_pid": None, "metrics": metrics, "sample_time": timestamp, **values}


def main():
    """按 UUID 发布整卡指标；无权限或驱动不可用显示缺失状态。"""
    emitter = Emitter("monitor-" + str(os.getpid()))
    try:
        import pynvml as nvml
        nvml.nvmlInit()
        handles = [nvml.nvmlDeviceGetHandleByIndex(i) for i in range(nvml.nvmlDeviceGetCount())]
    except Exception as exc:
        emitter.send("telemetry.sample", payload("unavailable", str(exc)))
        return 0
    try:
        while True:
            for index, handle in enumerate(handles):
                try:
                    memory = nvml.nvmlDeviceGetMemoryInfo(handle)
                    utilization = nvml.nvmlDeviceGetUtilizationRates(handle)
                    emitter.send("telemetry.sample", payload("available", index=index,
                        uuid=nvml.nvmlDeviceGetUUID(handle), name=nvml.nvmlDeviceGetName(handle),
                        used=memory.used, total=memory.total, utilization=utilization.gpu))
                except Exception as exc:
                    emitter.send("telemetry.sample", payload("error", str(exc), index=index))
            time.sleep(1)
    except (BrokenPipeError, KeyboardInterrupt):
        pass
    finally:
        nvml.nvmlShutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
