# Standard Library

Stasha's standard library (`stsstdlib/`) is written in Stasha itself. Build it with `make stdlib` before using `from std` imports.

---

## Using the Standard Library

```bash
make stdlib   # build all stdlib modules into bin/stdlib/
```

Then in your code:
```stasha
libimp "array"  from std;
libimp "dstring" from std;
libimp "map"    from std;
```

---

## Collections

### `array` — Dynamic Array

```stasha
libimp "array" from std;

array_t.[i32] arr = array_t.[i32].new(8);
defer arr.rem();

arr.push(10);
arr.push(20);
arr.push(30);

i32 first = arr.at(0);          // 10
arr.set(1, 99);                  // replace index 1
i32 len = arr.len;               // 3
arr.insert(1, 50);               // insert 50 at index 1
arr.remove(2);                   // remove index 2
arr.reverse();
arr.ensure_capacity(100);        // pre-allocate
```

### `list` — Doubly Linked List

```stasha
libimp "list" from std;

list_t.[i32] l = list_t.[i32].new();
defer l.rem();

l.push_back(1);
l.push_back(2);
l.push_front(0);
// Iterate with foreach or iterator
```

### `set` — Hash Set

```stasha
libimp "set" from std;

set_t.[i32] s = set_t.[i32].new();
defer s.rem();

s.insert(42);
bool found = s.contains(42);    // true
s.remove(42);
```

### `opt` — Optional Value

```stasha
libimp "opt" from std;

opt_t.[i32] maybe = opt_t.[i32].from_some(42);
opt_t.[i32] none  = opt_t.[i32].from_none();

bool has = maybe.is_some();      // true
let [ptr, err] = maybe.get();    // ptr is ?i32 *r, err is nil on Some
if err == nil {
    print.('{}\n', ptr[0]);      // 42
}
bool eq = maybe.equ(&none);      // false
maybe.set(100);
maybe.set_none();
```

### `buffer` — Ring Buffer

```stasha
libimp "buffer" from std;

buffer_t.[i32] b = buffer_t.[i32].new(16);
defer b.rem();

b.push(1);
b.push(2);
i32 v = b.pop();   // 1 (FIFO)
```

---

## Map

### `map` — Hash Map

```stasha
libimp "map" from std;

map_t.[i32, bool] m = map_t.[i32, bool].new();
defer m.rem();

m.insert(1, true);
m.insert(2, false);

let [val, err] = m.get(1);      // val is bool *r, err is nil when found
if err == nil {
    print.('{}\n', val[0]);     // true
}
stack bool *r maybe = m.get_or_nil(2);
bool exists = m.contains(2);    // true
m.remove(1);
```

---

## Strings

### `string` — String Utilities

```stasha
libimp "string" from std;

// Comparison, search, etc.
bool eq = string.equals("hello", "hello");
i32 idx = string.find("hello world", "world");
```

### `dstring` — Dynamic (Mutable) String

```stasha
libimp "dstring" from std;

dstring_t s = dstring_t.new("hello");
defer s.rem();

s.append(" world");
s.append_char('!');
print.('{}\n', s.as_str());   // "hello world!"

i32 len = s.len;
bool starts = s.starts_with("hello");
bool ends   = s.ends_with("!");
```

### `str_view` — Immutable String View

```stasha
libimp "str_view" from std;

str_view_t v = str_view_t.from("hello world");
str_view_t sub = v.slice(6, 11);   // "world"
bool eq = sub.equals("world");
```

### `str_conv` — String Conversions

```stasha
libimp "str_conv" from std;

i32  n = str_conv.to_int("42");
f64  f = str_conv.to_float("3.14");

heap i8 *rw s = str_conv.from_int(42);
defer rem.(s);
```

### `unicode` — UTF-8 Support

```stasha
libimp "unicode" from std;

u32  cp  = unicode.decode_codepoint(bytes, &advance);
i32  len = unicode.codepoint_len(cp);
bool ok  = unicode.is_valid_utf8(bytes, n);
```

---

## Math

### `math` — Math Functions

```stasha
libimp "math" from std;

f64 s = math.sin(3.14);
f64 c = math.cos(0.0);
f64 r = math.sqrt(2.0);
f64 p = math.pow(2.0, 10.0);
f64 l = math.log(2.718);
f64 a = math.abs(-5.0);
f64 fl = math.floor(3.7);
f64 cl = math.ceil(3.2);
```

### `vector` — Vector Math

```stasha
libimp "vector" from std;

math.Vec2 a = .{ .x = 1.0, .y = 0.0 };
math.Vec2 b = .{ .x = 0.0, .y = 1.0 };
f64 d = math.dot2(a, b);       // 0.0
f64 l = math.len2(a);          // 1.0
math.Vec2 n = math.normalize2(a);
```

---

## I/O

### `console` — Console I/O

```stasha
libimp "console" from std;

console.print("hello\n");
console.print_err("error\n");
```

Or use the built-in `print.()` — no import needed.

### `file_stream` — File I/O

```stasha
libimp "file_stream" from std;

let [stream, err] = file_stream_t.open("data.txt", "r");
if err != nil { handle(err); }
defer stream.close();

let [line, err2] = stream.read_line();
let [n, err3]    = stream.read_bytes(buf, 256);
stream.write_bytes(data, len);
```

### `logging` — Structured Logging

```stasha
libimp "logging" from std;

logger_t log = logger_t.new("myapp");
log.info("server started on port {}", port);
log.warn("connection timeout after {}ms", ms);
log.error("fatal: {}", msg);
```

---

## Time

### `clock` — Timing

```stasha
libimp "clock" from std;

u64 start = clock.now_ns();
do_work();
u64 elapsed = clock.now_ns() - start;
print.('elapsed: {}ms\n', elapsed / 1_000_000);
```

### `time.extra` — Delays

```stasha
libimp "extra" from std;   // from time/

time.sleep_ms(100);   // sleep 100 milliseconds
time.sleep_us(500);   // sleep 500 microseconds
```

---

## Threading

### `mutex` — Mutual Exclusion

```stasha
libimp "mutex" from std;

threading.mutex_t m = threading.mutex_t.new();
defer m.rem();

m.lock();
// critical section
m.unlock();

// With defer:
m.lock();
defer m.unlock();
// ...
```

---

## System

### `sys` — OS Utilities

```stasha
libimp "sys" from std;

i32 exit_code = sys.exit_code();
stack i8 *r   home = sys.env("HOME");
i32 pid = sys.getpid();
```

### `cl_args` — Command-Line Arguments

```stasha
libimp "cl_args" from std;

cl_args_t args = cl_args_t.new(argc, argv);

// Access arguments:
stack i32 count = args.count();
stack i8 *r name = args.get(0);

// Flags:
bool verbose = args.flag("--verbose");
stack i8 *r output = args.option("--output");
```

---

## Network

### `network` — TCP/UDP Sockets

```stasha
libimp "network" from std;

// TCP server
let [server, err] = net.tcp_listen("0.0.0.0", 8080);
let [client, err2] = server.accept();
client.send(data, len);
let [n, err3] = client.recv(buf, 256);
```

### `http` — HTTP Server/Client

```stasha
libimp "http" from std;

// HTTP server (powered by Mongoose):
http_server_t srv = http_server_t.new(8080);
srv.route("GET", "/hello", fn handle_hello);
srv.run();

// HTTP client:
let [resp, err] = http.get("https://example.com/api");
print.('{}\n', resp.body);
```

---

## JSON

```stasha
libimp "json" from std;

// Parse:
let [doc, err] = json.parse(json_string);
stack i32 n = doc.get_int("count");
stack i8 *r s = doc.get_str("name");

// Build:
json_builder_t b = json_builder_t.new();
b.begin_object();
b.field_int("x", 42);
b.field_str("name", "Alice");
b.end_object();
heap i8 *rw out = b.finish();
defer rem.(out);
```

---

## Random

### `simple_rng` — Fast RNG

```stasha
libimp "simple_rng" from std;

simple_rng_t rng = simple_rng_t.new(seed);
i32 n  = rng.next_i32();
f64 f  = rng.next_f64();         // [0.0, 1.0)
i32 r  = rng.range(1, 100);      // [1, 100]
bool b = rng.bool();
```

### `complex_rng` — Cryptographic RNG

```stasha
libimp "complex_rng" from std;

// Cryptographically secure (uses OpenSSL):
complex_rng_t rng = complex_rng_t.new();
i32 secure = rng.next_i32();
```

---

## Process

```stasha
libimp "process" from std;

let [proc, err] = proc.spawn("ls", .{"-la"});
proc.wait();
stack i32 code = proc.exit_code();
heap i8 *rw out = proc.stdout();
defer rem.(out);
```

---

## Configuration

```stasha
libimp "config" from std;

let [cfg, err] = config.load("app.toml");
stack i32 port  = cfg.get_int("server.port");
stack i8 *r host = cfg.get_str("server.host");
bool debug = cfg.get_bool("app.debug");
```

---

## Crypto

```stasha
libimp "crypto" from std;

// SHA-256:
heap u8 *rw hash = crypto.sha256(data, len);
defer rem.(hash);

// HMAC:
heap u8 *rw mac = crypto.hmac_sha256(key, key_len, data, data_len);
defer rem.(mac);
```

---

## Filesystem

```stasha
libimp "filesystem" from std;

bool exists = fs.exists("path/to/file");
let [content, err] = fs.read_file("data.txt");
defer rem.(content);
let [_, err2] = fs.write_file("out.txt", data, len);
let [entries, err3] = fs.list_dir(".");
```
