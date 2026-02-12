fn main() {
    println!("Hello, world!");
}
#[cfg(test)]
mod tests {
    use std::{
        io::{Read, Write},
        net::{TcpListener, TcpStream},
        process::Command,
        time::Duration,
    };

    #[test]
    fn test_forward_request() {
        let _ = std::thread::spawn(|| {
            let listener = TcpListener::bind("127.0.0.1:9090").unwrap();
            let (mut stream, _) = listener.accept().unwrap();

            let mut buf: [u8; 5] = [0; 5];
            stream.read_exact(&mut buf).unwrap();

            stream.write_all("world".as_bytes()).unwrap();
        });

        let mut kroxy = Command::new("./kroxy")
            .arg("./http.config.json")
            .spawn()
            .unwrap();
        std::thread::sleep(Duration::from_secs(2));

        let mut sock = TcpStream::connect("127.0.0.1:8080").unwrap();
        sock.write_all("hello".as_bytes()).unwrap();
        let mut buf: [u8; 5] = [0; 5];
        sock.read_exact(&mut buf).unwrap();
        assert_eq!(String::from_utf8_lossy(&buf), "world");

        kroxy.kill().unwrap();
    }
}
