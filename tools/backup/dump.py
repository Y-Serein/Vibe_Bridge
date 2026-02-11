#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import serial
import threading
import queue
import signal
import sys
import time

stop_flag = False


def reader_thread(ser, q, chunk_size):
    while not stop_flag:
        try:
            data = ser.read(chunk_size)
            if data:
                q.put(data)
        except Exception as e:
            print(f"Serial read error: {e}", file=sys.stderr)
            break


def consumer_thread(q, stats):
    syncing = False
    cur_len = 0

    total_bytes = 0
    total_pkts = 0

    while not stop_flag or not q.empty():
        try:
            data = q.get(timeout=0.1)
        except queue.Empty:
            continue

        total_bytes += len(data)

        i = 0
        n = len(data)
        while i < n:
            pos = data.find(b'\xFF', i)
            if pos < 0:
                if syncing:
                    cur_len += n - i
                break

            if syncing:
                total_pkts += 1
                stats["min_pkt"] = min(stats["min_pkt"], cur_len)
                stats["max_pkt"] = max(stats["max_pkt"], cur_len)

            syncing = True
            cur_len = 1
            i = pos + 1

    stats["bytes"] = total_bytes
    stats["pkts"] = total_pkts


def signal_handler(sig, frame):
    global stop_flag
    stop_flag = True


def main():
    if len(sys.argv) < 3:
        print("Usage: python uart_stat.py COM5 3000000")
        sys.exit(1)

    port = sys.argv[1]       # e.g. COM5
    baud = int(sys.argv[2])  # e.g. 3000000

    ser = serial.Serial(
        port=port,
        baudrate=baud,
        bytesize=serial.EIGHTBITS,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=None,        # blocking
        write_timeout=None,
    )

    # 尝试扩大 Windows 驱动缓冲（不保证生效，但有益）
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

    t1 = threading.Thread(target=reader_thread, args=(ser, q, 16384), daemon=True)
    t2 = threading.Thread(target=consumer_thread, args=(q, stats), daemon=True)

    t1.start()
    t2.start()

    start = time.time()
    print(f"Opened {port} @ {baud} baud")

    while not stop_flag:
        time.sleep(0.5)

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
