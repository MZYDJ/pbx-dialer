#!/usr/bin/env python
# 本地 MQTT Broker (amqtt) — 仅用于离线自测, 绑定 127.0.0.1:1883
import asyncio
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "testlibs"))

from amqtt.broker import Broker  # noqa: E402


async def main() -> None:
    config = {
        "listeners": {"default": {"type": "tcp", "bind": "127.0.0.1:1883"}},
        "sys_interval": 0,
    }
    broker = Broker(config)
    await broker.start()
    print("[broker] listening on 127.0.0.1:1883", flush=True)
    try:
        while True:
            await asyncio.sleep(1)
    except (KeyboardInterrupt, asyncio.CancelledError):
        pass
    finally:
        await broker.shutdown()
        print("[broker] stopped", flush=True)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
