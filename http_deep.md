## Web Server Behavior, HTTP over TCP, and PCAP Analysis

## Learning Objectives

By the end of this session, students will be able to:

1. Explain how a web server actually serves content over the network, from TCP connection setup to HTTP request/response.
2. Describe the historical evolution of HTTP (0.9 → 1.0 → 1.1 → 2 → 3) and why that matters for performance, security, and packet structure.
3. Explain how HTTP runs on top of TCP (3-way handshake, ports, connection reuse/persistence).
4. Capture and analyze HTTP/1.1 traffic in a PCAP using Wireshark or tcpdump.
5. Identify sensitive information in plaintext HTTP (credentials, cookies, session tokens) and connect that to security risk.

This session blends protocol theory and hands-on forensic-style packet inspection.

---

## 1. End-to-end Flow: Browser to Web Server

When a browser accesses `http://example.com/`, the flow is:

1. **DNS Resolution**
   The client (browser) resolves the domain name to an IP address.

2. **TCP 3-Way Handshake to the Web Server**

   * Client → Server: `SYN`
   * Server → Client: `SYN, ACK`
   * Client → Server: `ACK`
     After this handshake, the TCP connection is established (typically to port 80 for HTTP).

3. **HTTP Request**
   The browser sends an HTTP request (for example `GET /index.html HTTP/1.1` or `POST /login HTTP/1.1`).

4. **HTTP Response**
   The web server replies with a response (`HTTP/1.1 200 OK`, headers, and body such as HTML or JSON).

5. **Connection handling**
   Depending on the HTTP version and headers, the TCP connection may be:

   * closed immediately, or
   * kept alive and reused for additional requests.

Note to students:

* We will zoom in on steps 2–4 using packet capture.
* You should be able to draw this as a sequence diagram (Client ↔ Server) including TCP and HTTP.

---

## 2. Evolution of HTTP (Why Your PCAP Looks the Way It Looks)

Understanding HTTP versions helps you interpret traffic in Wireshark.

### HTTP/0.9

* Extremely simple.
* Only supported a single `GET` with no headers.
* The server just returned raw HTML, then closed the connection.
* One request = one connection.

### HTTP/1.0

* Introduced headers (like `User-Agent`, `Content-Type`, etc.).
* Still typically used one TCP connection per request.
* After delivering the resource, the server would close the connection.

Impact:
Loading a page with 20 images meant ~20 separate TCP handshakes. Inefficient and high latency.

### HTTP/1.1

* Becomes the standard baseline of the Web.
* Adds **persistent connections / keep-alive by default**, so one TCP connection can serve multiple sequential requests.
* Introduces `Host:` header (so multiple domains can live behind one IP).
* Introduces chunked transfer, better caching, etc.
* Still human-readable plaintext over TCP (unless wrapped in TLS).

Impact:
In a PCAP of HTTP/1.1 without TLS, you can see:

* multiple requests and responses within the same TCP stream,
* credentials in cleartext if the site is not using HTTPS,
* cookies and session tokens.

This is what we will analyze in lab.

### HTTP/2

* Focuses on performance.
* Multiplexes many logical requests/responses over a single connection in parallel.
* Compresses headers.
* Almost always runs over TLS.
* Result: far less trivial to inspect in Wireshark without having the server’s TLS keys.

### HTTP/3

* Runs over QUIC (which runs over UDP), not TCP.
* Aims for lower latency and better behavior on unstable/roaming networks (mobile, Wi-Fi handoffs).
* Again, encrypted by default.

Summary for class:

* For hands-on traffic analysis and incident response fundamentals, HTTP/1.1 over TCP port 80 is the clearest starting point: readable, inspectable, and directly shows security risk.
* Modern production traffic tends to be HTTPS (HTTP over TLS on port 443), HTTP/2, or HTTP/3 — which means payload inspection is no longer trivial.

---

## 3. TCP Fundamentals for HTTP

HTTP (application layer) assumes a reliable byte stream. TCP (transport layer) provides that reliability.

### TCP 3-Way Handshake

Before any HTTP data is sent:

1. Client sends `SYN` to the server.
2. Server replies with `SYN, ACK`.
3. Client replies with `ACK`.

Only after that does the client send the HTTP request.

In Wireshark:

* You will see this handshake before the first visible HTTP packet.
* You can estimate initial latency (RTT) from SYN → SYN/ACK timing.

### Ports

* Classic HTTP: TCP destination port 80 (plaintext).
* HTTPS: TCP destination port 443 (TLS-encrypted HTTP).
* Note: these are conventions, not hard rules, but most captures will follow them.

### TCP Stream Identity

In Wireshark you’ll often group packets by TCP stream. A TCP stream is identified by:

* client IP:client port ↔ server IP:server port

Wireshark’s “Follow TCP Stream” feature reconstructs the full bidirectional conversation in order, which is perfect for reading HTTP/1.1.

---

## 4. HTTP Request/Response on the Wire

After TCP is established, the browser sends an HTTP request. Let’s look at typical plaintext HTTP/1.1.

### Example HTTP Request (client → server)

```http
POST /login HTTP/1.1
Host: demo.local
User-Agent: LabBrowser/1.0
Content-Type: application/x-www-form-urlencoded
Content-Length: 30
Connection: keep-alive

username=alice&password=lab123
```

Important elements:

* **Request line:** `POST /login HTTP/1.1`

  * Method (`POST`, `GET`, etc.)
  * Path (`/login`)
  * Version (`HTTP/1.1`)
* **Headers:** Metadata about the request, client, content encoding, etc.
* **Body:** The actual data (in this case credentials).

Security note:

* In HTTP (not HTTPS), credentials like `username=alice&password=lab123` are sent in cleartext.
* Anyone capturing traffic on the path can read this.

### Example HTTP Response (server → client)

```http
HTTP/1.1 200 OK
Date: Fri, 31 Oct 2025 10:00:00 GMT
Server: demo-httpd/1.0
Content-Type: text/html; charset=UTF-8
Set-Cookie: sessionid=ABC123XYZ; HttpOnly
Content-Length: 39

<html><body>Welcome alice</body></html>
```

Important elements:

* **Status line:** `HTTP/1.1 200 OK`

  * HTTP version
  * Status code (200, 404, 500, etc.)
* **Headers:** e.g. `Content-Type`, `Content-Length`, `Set-Cookie`, etc.
* **Body:** The actual page content or API response.

Security note:

* The `Set-Cookie:` header is extremely sensitive. If an attacker steals that session token, they may be able to hijack the user’s session.
* If this is over plaintext HTTP, it is trivially sniffable.

### Persistent Connection / Keep-Alive

Notice the header:

```http
Connection: keep-alive
```

In HTTP/1.1, persistent connections are default. The client and server can reuse the same TCP connection for further requests, instead of tearing it down immediately. This reduces repeated handshakes and improves performance.

In the PCAP, you’ll see:

* one TCP handshake,
* then multiple HTTP exchanges back-to-back,
* and only later (or sometimes not immediately) a FIN/ACK to close the connection.

---

## 5. What the Web Server Actually Does

On the server side (e.g. Nginx, Apache, a custom HTTP daemon):

1. Listens on TCP port 80 (or 443 if HTTPS).
2. Accepts an incoming TCP connection.
3. Reads the HTTP request.
4. Decides how to respond:

   * Serve a static file (HTML, CSS, image).
   * Call application logic (e.g. pass request to backend service / app server / FastCGI / reverse proxy).
5. Builds an HTTP response (status code, headers, body).
6. Sends it back over the same TCP socket.
7. Either keeps the TCP connection open (persistent) or closes it.

Important to highlight:

* In production, the “web server” is often not the business logic itself.
  It’s frequently acting as a reverse proxy / load balancer / TLS terminator / rate limiter / WAF entry point.
* That’s why packet captures at the edge are valuable for incident response.

---

## 6. Lab: HTTP/1.1 Traffic Capture and PCAP Analysis

In the lab, students will work with a provided PCAP file (`sample_http.pcap`) that simulates a login over HTTP/1.1 without TLS. ( Link: https://github.com/ferryas-pens/Advanced_Network_Concept/blob/main/sample_http.pcap)

### Step A. Capturing (for students who want to try themselves)

On a lab machine or VM, you could do:

```bash
tcpdump -i <interface> -s 0 -w capture_http.pcap port 80
```

Explanation:

* `-i <interface>`: choose the active NIC (e.g. `eth0`).
* `-s 0`: capture full packets (no truncation).
* `-w capture_http.pcap`: write to a file for later analysis.
* `port 80`: focus only on HTTP plaintext to avoid noise.

### Step B. Opening the PCAP in Wireshark

1. Open the `sample_http.pcap` file in Wireshark 
2. Apply a display filter:
   `http`
   This shows packets Wireshark recognizes as HTTP.
3. Identify the TCP 3-way handshake (SYN, SYN/ACK, ACK) between the client (e.g. `10.0.0.10`) and the server (`10.0.0.80:80`).
4. Right-click one of the HTTP packets → **Follow** → **TCP Stream**.

   * Wireshark will reconstruct a readable conversation (client requests and server responses in order).

### Step C. What to Extract

From the “Follow TCP Stream” view, students must locate:

* The HTTP request method and path:

  * e.g. `POST /login HTTP/1.1`
* The `Host:` header (virtual host / site name)
* The `User-Agent:` header (what client claims to be)
* The request body containing credentials:

  * `username=alice&password=lab123`
* The server’s HTTP status code:

  * e.g. `HTTP/1.1 200 OK`
* The `Set-Cookie:` header from the server:

  * e.g. `sessionid=ABC123XYZ; HttpOnly`
* The HTML response body (`Welcome alice`)

### Step D. Persistent Connection Check

Look at the end of the stream:

* Does the server immediately close the TCP connection with `FIN`?
* Or is the connection left open (`Connection: keep-alive`) for potential reuse?

This demonstrates HTTP/1.1 persistent connections.

---

## 7. Security Discussion (Tie-in to Network Defense / Forensics)

Key talking points to drive home:

1. **Plaintext HTTP is dangerous.**
   Anyone who can sniff traffic between client and server can read:

   * login credentials in POST bodies,
   * session cookies in `Set-Cookie:`,
   * internal endpoints or admin URLs.

2. **HTTPS (HTTP over TLS) protects confidentiality.**
   With HTTPS, the actual HTTP payload is encrypted, so you can still see the TCP/TLS handshake and SNI-type metadata, but not the raw credentials or response body.
   This is why modern production environments enforce HTTPS for login flows.

3. **Forensics trade-off.**

   * Plain HTTP is easy to inspect, but insecure.
   * Encrypted HTTPS is secure, but harder for defenders to inspect at the network layer.
     In many enterprises, TLS is terminated at a controlled reverse proxy / inspection point so that security teams can still apply policy, logging, and detection.

4. **Modern protocols move away from “HTTP over TCP only.”**

   * HTTP/2 multiplexes and is normally encrypted.
   * HTTP/3 runs over QUIC/UDP.
   * This means traditional “Follow TCP Stream and read everything” is no longer always possible in production traffic.

The PCAP we give you is intentionally old-school (HTTP/1.1 over TCP port 80, no TLS) so you can learn the fundamentals clearly.

---

## 8. Assignment (Deliverables to Submit)

Students must submit answers to the following tasks:

**Task 1. Sequence Diagram**
Draw a sequence diagram of the full exchange from the PCAP:

* TCP SYN → SYN/ACK → ACK
* `POST /login HTTP/1.1` request
* `HTTP/1.1 200 OK` response with `Set-Cookie`
  Include client IP and server IP.

**Task 2. Sensitive Data Identification**
From the captured HTTP conversation:

* Highlight at least three sensitive elements (for example: plaintext password in the POST body, `Authorization:` header if present, `Set-Cookie:` session token).
* For each, explain the security impact if an attacker on the same network captures this traffic.

**Task 3. HTTP/1.0 vs HTTP/1.1 Connection Behavior**
In your own words:

* Why is HTTP/1.0 considered “one request per TCP connection”?
* Why does HTTP/1.1 default to “keep-alive” / persistent connections?
* How does that improve performance?


**Ethics / Policy Reminder for Students:**

* Only capture traffic in controlled lab environments.
* Do not capture or submit real credentials belonging to other users.
* If your capture includes a real session token or password, redact/blur it in the screenshot before submitting your report.

---

## 9. PCAP Distribution Notes

The lab PCAP is called `sample_http.pcap`.
It contains:

* A TCP handshake between a client `10.0.0.10:54321` and a web server `10.0.0.80:80`.
* An HTTP/1.1 `POST /login` request with plaintext credentials:
  `username=alice&password=lab123`
* A server response `HTTP/1.1 200 OK` that includes:

  * `Set-Cookie: sessionid=ABC123XYZ; HttpOnly`
  * HTML body: `<html><body>Welcome alice</body></html>`
* `Connection: keep-alive`, demonstrating persistent HTTP/1.1 behavior (the connection does not immediately close).

If students cannot download directly from the LMS environment, they can reconstruct the PCAP file locally from a provided Base64 blob (we’ve prepared that). After decoding, they open it in Wireshark and proceed with the analysis steps above.

---

## 10. Reference Reading for Students

These sources are recommended pre/post session:

1. HTTP Evolution / History

   * How HTTP evolved from simple single-request connections to multiplexed encrypted connections (HTTP/2, HTTP/3).
   * Why HTTP/1.1 introduced persistent connections and `Host:` headers.

2. RFC / HTTP/1.1 Semantics

   * Request line / status line
   * Headers (`Host`, `Content-Type`, `Content-Length`, `Set-Cookie`, etc.)
   * Connection persistence (`Connection: keep-alive`)

3. Wireshark Basics

   * Using `tcpdump` to capture.
   * Opening the PCAP in Wireshark.
   * Display filter `http`.
   * “Follow TCP Stream” to reconstruct the conversation.

4. Security Angle

   * Credential exposure over plaintext HTTP.
   * Cookie/session hijacking.
   * Why TLS (HTTPS) is mandatory in modern environments.

---
## Referensi :
1. Evolusi HTTP (0.9 → 1.1 → 2 → 3)
   [https://developer.mozilla.org/en-US/docs/Web/HTTP/Guides/Evolution_of_HTTP](https://developer.mozilla.org/en-US/docs/Web/HTTP/Guides/Evolution_of_HTTP)  ([developer.mozilla.org][1])
2. RFC 2616 – HTTP/1.1 klasik (struktur request/response, header, semantics)
   [https://datatracker.ietf.org/doc/html/rfc2616](https://datatracker.ietf.org/doc/html/rfc2616)  ([datatracker.ietf.org][2])
3. Ringkas sejarah HTTP hingga HTTP/3 (2025 insight)
   [https://www.innoq.com/en/blog/2025/04/a-brief-history-of-http/](https://www.innoq.com/en/blog/2025/04/a-brief-history-of-http/)  ([innoq.com][7])
4. HTTP in Depth (perbedaan 1.0 vs 1.1 vs keep-alive)
   [https://medium.com/%40ahmadfarag/http-in-depth-dfdac806c2c0](https://medium.com/%40ahmadfarag/http-in-depth-dfdac806c2c0)  ([Medium][10])
5. Wireshark User Guide (dasar, cara baca paket, follow stream)
   [https://www.wireshark.org/docs/wsug_html/](https://www.wireshark.org/docs/wsug_html/)  ([Wireshark][4])
6. Follow TCP Stream di Wireshark (langkah demi langkah)
   [https://www.wireshark.org/docs/wsug_html_chunked/ChAdvFollowStreamSection.html](https://www.wireshark.org/docs/wsug_html_chunked/ChAdvFollowStreamSection.html)  ([Wireshark][3])
7. Analisis TCP Handshake dan Stream di Wireshark (contoh praktis + keamanan)
   [https://www.geeksforgeeks.org/ethical-hacking/tcp-analysis-using-wireshark/](https://www.geeksforgeeks.org/ethical-hacking/tcp-analysis-using-wireshark/)  ([geeksforgeeks.org][5])
8. Filter Wireshark untuk analisis trafik dan ancaman
   [https://www.freecodecamp.org/news/use-wireshark-filters-to-analyze-network-traffic/](https://www.freecodecamp.org/news/use-wireshark-filters-to-analyze-network-traffic/)  ([freecodecamp.org][11])
9. HTTP/3 dan QUIC (bagaimana web modern pindah dari TCP ke UDP/QUIC)
   [https://blog.cloudflare.com/http-3-from-root-to-tip/](https://blog.cloudflare.com/http-3-from-root-to-tip/)  ([blog.cloudflare.com][12])

[1]: https://developer.mozilla.org/en-US/docs/Web/HTTP/Guides/Evolution_of_HTTP?utm_source=chatgpt.com "Evolution of HTTP - MDN Web Docs"
[2]: https://datatracker.ietf.org/doc/html/rfc2616?utm_source=chatgpt.com "RFC 2616 - Hypertext Transfer Protocol -- HTTP/1.1"
[3]: https://www.wireshark.org/docs/wsug_html_chunked/ChAdvFollowStreamSection.html?utm_source=chatgpt.com "7.2. Following Protocol Streams"
[4]: https://www.wireshark.org/docs/wsug_html/?utm_source=chatgpt.com "Wireshark User's Guide"
[5]: https://www.geeksforgeeks.org/ethical-hacking/tcp-analysis-using-wireshark/?utm_source=chatgpt.com "TCP Analysis using Wireshark"
[6]: https://en.wikipedia.org/wiki/HTTP?utm_source=chatgpt.com "HTTP"
[7]: https://www.innoq.com/en/blog/2025/04/a-brief-history-of-http/?utm_source=chatgpt.com "A Brief History of HTTP"
[8]: https://www.wireshark.org/docs/wsug_html_chunked/ChAdvTCPAnalysis.html?utm_source=chatgpt.com "7.5. TCP Analysis"
[9]: https://labex.io/tutorials/wireshark-analyze-tcp-traffic-with-wireshark-follow-tcp-stream-feature-415946?utm_source=chatgpt.com "Analyze TCP Traffic with Wireshark Follow TCP Stream ..."
[10]: https://medium.com/%40ahmadfarag/http-in-depth-dfdac806c2c0?utm_source=chatgpt.com "HTTP In Depth. HTTP | by Ahmad Farag"
[11]: https://www.freecodecamp.org/news/use-wireshark-filters-to-analyze-network-traffic/?utm_source=chatgpt.com "How to Use Wireshark Filters to Analyze Your Network Traffic"
[12]: https://blog.cloudflare.com/http-3-from-root-to-tip/?utm_source=chatgpt.com "HTTP/3: From root to tip"
