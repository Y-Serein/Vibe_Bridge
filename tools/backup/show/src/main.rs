use crossbeam_channel::{bounded, Receiver, Sender};
use serialport::SerialPort;
use std::env;
use std::io::Read;
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};
use std::thread;
use std::time::Duration;

use show_image::{create_window, ImageInfo};
use image::{GrayImage, Rgb, RgbImage};

const FRAME_W: usize = 160;
const FRAME_H: usize = 120;
const FRAME_PIXELS: usize = FRAME_W * FRAME_H;

/* 串口读取线程：只负责 IO */
fn reader_thread(
    mut port: Box<dyn SerialPort>,
    tx: Sender<Vec<u8>>,
    stop: Arc<AtomicBool>,
    chunk: usize,
) {
    let mut buf = vec![0u8; chunk];

    while !stop.load(Ordering::Relaxed) {
        match port.read(&mut buf) {
            Ok(n) if n > 0 => {
                let mut v = Vec::with_capacity(n);
                v.extend_from_slice(&buf[..n]);
                if tx.send(v).is_err() {
                    break;
                }
            }
            Ok(_) => {}
            Err(_) => break,
        }
    }

    println!("Reader thread exit");
}

/* 组包线程：从字节流中提取完整帧 */
fn packet_assembler(
    rx: Receiver<Vec<u8>>,
    frame_tx: Sender<Vec<u8>>,
    stop: Arc<AtomicBool>,
) {
    let mut buf = Vec::with_capacity(FRAME_PIXELS);
    let mut in_packet = false;
    let mut frame_cnt: u64 = 0;

    while !stop.load(Ordering::Relaxed) {
        let data = match rx.recv_timeout(Duration::from_millis(100)) {
            Ok(d) => d,
            Err(_) => continue,
        };

        for &b in &data {
            if !in_packet {
                if b == 0xFF {
                    buf.clear();
                    in_packet = true;
                }
                continue;
            }

            buf.push(b);

            if buf.len() == FRAME_PIXELS {

                frame_cnt += 1;
                if frame_cnt % 30 == 0 {
                    dump_frame_debug(frame_cnt, &buf);
                }

                let frame = std::mem::take(&mut buf);
                if frame_tx.send(frame).is_err() {
                    return;
                }
                in_packet = false;
            }
        }
    }

    println!("Packet assembler exit");
}

#[show_image::main]
fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 3 {
        eprintln!("Usage: {} <device> <baudrate> [chunk]", args[0]);
        return;
    }

    let device = &args[1];
    let baudrate: u32 = args[2].parse().expect("Invalid baudrate");
    let chunk: usize = args
        .get(3)
        .and_then(|v| v.parse().ok())
        .unwrap_or(16384);

    let stop = Arc::new(AtomicBool::new(false));
    let stop2 = stop.clone();

    ctrlc::set_handler(move || {
        stop2.store(true, Ordering::Relaxed);
    })
    .expect("Failed to set Ctrl-C handler");

    let port = serialport::new(device, baudrate)
        .timeout(Duration::from_millis(100))
        .open()
        .expect("Failed to open serial port");

    let (raw_tx, raw_rx) = bounded::<Vec<u8>>(32);
    let (frame_tx, frame_rx) = bounded::<Vec<u8>>(4);

    thread::spawn({
        let stop = stop.clone();
        let tx = raw_tx.clone();
        move || reader_thread(port, tx, stop, chunk)
    });

    thread::spawn({
        let stop = stop.clone();
        move || packet_assembler(raw_rx, frame_tx, stop)
    });

    /* GUI：必须在主线程 */
    let window = create_window("Grayscale", Default::default())
        .expect("Failed to create window");

    println!("Opened {} @ {} baud", device, baudrate);
    println!("Running... Press Ctrl-C to stop");

    while !stop.load(Ordering::Relaxed) {
        match frame_rx.recv_timeout(Duration::from_millis(50)) {
            Ok(frame) => {
                if frame.len() != FRAME_PIXELS {
                    continue;
                }

                /* Vec<u8> -> GrayImage（取得所有权） */
                let img = GrayImage::from_raw(
                    FRAME_W as u32,
                    FRAME_H as u32,
                    frame,
                ).expect("Invalid frame size");

                /* show_image 自动接管生命周期 */
                if window.set_image("frame", img).is_err() {
                    break;
                }
            }
            Err(_) => {}
        }
    }

    println!("Exit");
}

fn dump_frame_debug(cnt: u64, frame: &[u8]) {
    let min = frame.iter().copied().min().unwrap_or(0);
    let max = frame.iter().copied().max().unwrap_or(0);

    println!(
        "[frame {}] len={} min={} max={}",
        cnt,
        frame.len(),
        min,
        max
    );

    print!("  head: ");
    for b in frame.iter().take(32) {
        print!("{:02X} ", b);
    }
    println!();

    print!("  tail: ");
    for b in frame.iter().rev().take(32).rev() {
        print!("{:02X} ", b);
    }
    println!();
}
