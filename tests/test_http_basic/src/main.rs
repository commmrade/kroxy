fn main() {
    println!("Hello, world!");
}
#[cfg(test)]
mod tests {
    use axum::{Router, routing::get};
    use reqwest;
    use std::{thread, time::Duration};
    use tokio::runtime::Runtime;

    #[test]
    fn test_http_reverse_proxy() {
        // Start a tokio runtime for async axum server
        let rt = Runtime::new().unwrap();

        // Spawn backend server
        let backend_handle = thread::spawn(move || {
            let rt = Runtime::new().unwrap();
            rt.block_on(async {
                // Simple axum handler returning "world"
                async fn hello() -> &'static str {
                    "world"
                }

                let app = Router::new().route("/", get(hello));
                let listener = tokio::net::TcpListener::bind("127.0.0.1:9090")
                    .await
                    .unwrap();
                println!("listening on {}", listener.local_addr().unwrap());
                axum::serve(listener, app).await.unwrap();
            });
        });

        // Give backend some time to start
        thread::sleep(Duration::from_millis(1000));

        // Spawn your proxy
        let mut kroxy = std::process::Command::new("./kroxy")
            .arg("./http.config.json")
            .spawn()
            .expect("Failed to start proxy");

        // Give proxy some time to start
        thread::sleep(Duration::from_millis(2000));

        // Send HTTP request through the proxy using reqwest
        let client = reqwest::blocking::Client::new();
        let res = client.get("http://127.0.0.1:8080/").send().unwrap();

        let body = res.text().unwrap();
        assert!(!body.is_empty());

        // Clean up proxy
        kroxy.kill().unwrap();
        let _ = kroxy.wait();

        // Optionally, stop backend (thread will exit when process ends)
        // For more robust testing you could use a shutdown signal
    }
}
