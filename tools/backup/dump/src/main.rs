use crossbeam_channel::{Receiver, unbounded};
use serialport::SerialPort;
use std::env;
use std::io::Read;
use std::sync::{
    Arc,
    atomic::{AtomicBool, Ordering},
};
use std::thread;
use std::time::{Duration, Instant};

fn reader_thread(
    mut port: Box<dyn SerialPort>,
    tx: crossbeam_channel::Sender<Vec<u8>>,
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
}

fn parser_thread(rx: Receiver<Vec<u8>>, stop: Arc<AtomicBool>) {
    let mut in_packet = false;
    let mut cur_len: usize = 0;

    let mut total_bytes: usize = 0;
    let mut pkt_count: usize = 0;
    let mut min_pkt: usize = usize::MAX;
    let mut max_pkt: usize = 0;

    while !stop.load(Ordering::Relaxed) || !rx.is_empty() {
        let data = match rx.recv_timeout(Duration::from_millis(100)) {
            Ok(d) => d,
            Err(_) => continue,
        };

        total_bytes += data.len();

        for &b in data.iter() {
            if b == 0xFF {
                /* 遇到新包头，先结算上一个包 */
                if in_packet {
                    pkt_count += 1;
                    min_pkt = min_pkt.min(cur_len);
                    max_pkt = max_pkt.max(cur_len);
                }
                in_packet = true;
                cur_len = 1; /* 包头本身算 1 字节 */
            } else if in_packet {
                cur_len += 1;
            }
        }
    }

    println!("\n==== Packet Statistics ====");
    println!("Total bytes  : {}", total_bytes);
    println!("Packet count : {}", pkt_count);
    if pkt_count > 0 {
        println!("Min pkt size : {}", min_pkt);
        println!("Max pkt size : {}", max_pkt);
        println!(
            "Avg pkt size : {:.1}",
            total_bytes as f64 / pkt_count as f64
        );
    }
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 3 {
        eprintln!("Usage: {} <device> <baudrate> [chunk]", args[0]);
        return;
    }

    let device = &args[1];
    let baudrate: u32 = args[2].parse().expect("invalid baudrate");
    let chunk: usize = args.get(3).and_then(|v| v.parse().ok()).unwrap_or(16384);

    let stop = Arc::new(AtomicBool::new(false));
    let stop2 = stop.clone();

    ctrlc::set_handler(move || {
        stop2.store(true, Ordering::Relaxed);
    })
    .expect("failed to set Ctrl-C handler");

    let port = serialport::new(device, baudrate)
        .timeout(Duration::from_millis(100))
        .open()
        .expect("failed to open serial port");

    let (tx, rx) = unbounded::<Vec<u8>>();

    let start = Instant::now();

    let reader = {
        let stop = stop.clone();
        let tx = tx.clone();
        thread::spawn(move || reader_thread(port, tx, stop, chunk))
    };

    let parser = {
        let stop = stop.clone();
        thread::spawn(move || parser_thread(rx, stop))
    };

    println!("Opened {} @ {} baud", device, baudrate);
    println!("Running... Press Ctrl-C to stop");

    reader.join().ok();
    parser.join().ok();

    let elapsed = start.elapsed().as_secs_f64();
    println!("Elapsed time : {:.3} s", elapsed);
}
