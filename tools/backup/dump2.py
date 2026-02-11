#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import serial
import threading
import queue
import signal
import sys
import time

stop_flag = False
PACKET_LEN = 19201  # 每个数据包长度
CHUNK_SIZE = 32768  # 每次读入字节数，可以调大


def reader_thread(ser, q):
    """串口读取线程，将读到的数据放入队列"""
    global stop_flag
    while not stop_flag:
        try:
            data = ser.read(CHUNK_SIZE)
            if data:
                q.put(data)
        except Exception as e:
            print(f"Serial read error: {e}", file=sys.stderr)
            break


def consumer_thread(q, stats):
    """数据处理线程，统计完整包长度"""
    global stop_flag
    buf = bytearray()
    total_bytes = 0
    total_pkts = 0
    stats["min_pkt"] = 1 << 30
    stats["max_pkt"] = 0

    while not stop_flag or not q.empty():
        try:
            data = q.get(timeout=0.1)
        except queue.Empty:
            continue

        buf += data
        total_bytes += len(data)

        while True:
            pos = buf.find(b'\xFF')
            if pos < 0:
                # 没有包头，保留缓冲区
                buf.clear()
                break

            if len(buf) - pos >= PACKET_LEN:
                # 找到完整包
                pkt = buf[pos:pos + PACKET_LEN]
                total_pkts += 1
                stats["min_pkt"] = min(stats["min_pkt"], len(pkt))
                stats["max_pkt"] = max(stats["max_pkt"], len(pkt))
                # 删除已处理的包
                buf = buf[pos + PACKET_LEN:]
            else:
                # 包未完整，丢掉包头前的数据
                if pos > 0:
                    buf = buf[pos:]
                break

    stats["bytes"] = total_bytes
    stats["pkts"] = total_pkts


def signal_handler(sig, frame):
    global stop_flag
    stop_flag = True


def main():
    if len(sys.argv) < 3:
        print("Usage: python uart_stat.py COM5 3000000")
        sys.exit(1)

    port = sys.argv[1]
    baud = int(sys.argv[2])

    ser = serial.Serial(
        port=port,
        baudrate=baud,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=None,
        write_timeout=None,
    )

    # 尝试扩大 Windows 驱动缓冲区（不保证生效）
    try:
        ser.set_buffer_size(rx_size=1024 * 1024, tx_size=1024 * 1024)
    except Exception:
        pass

    q = queue.Queue()
    stats = {
        "bytes": 0,
        "pkts": 0,
        "min_pkt": 1 << 30,
        "max_pkt": 0,
    }

    signal.signal(signal.SIGINT, signal_handler)

    t1 = threading.Thread(target=reader_thread, args=(ser, q), daemon=True)
    t2 = threading.Thread(target=consumer_thread, args=(q, stats), daemon=True)

    t1.start()
    t2.start()

    start = time.time()
    print(f"Opened {port} @ {baud} baud, press Ctrl+C to stop")

    try:
        while not stop_flag:
            time.sleep(0.5)
    except KeyboardInterrupt:
        stop_flag = True

    elapsed = time.time() - start

    print("\n==== Statistics ====")
    print(f"Elapsed     : {elapsed:.2f}s")
    print(f"Total bytes : {stats['bytes']}")
    print(f"Total pkts  : {stats['pkts']}")
    if stats["pkts"]:
        print(f"Min pkt     : {stats['min_pkt']}")
        print(f"Max pkt     : {stats['max_pkt']}")
        print(f"Avg pkt     : {stats['bytes'] / stats['pkts']:.1f}")
    print(f"Throughput  : {stats['bytes'] / elapsed / 1024 / 1024:.2f} MB/s")


if __name__ == "__main__":
    main()
