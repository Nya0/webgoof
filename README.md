# webgoof
### DO NOT USE IN PRODUCTION
simple webserver built for fun and learning.

<img width="1290" height="926" alt="e-phosne" src="https://github.com/user-attachments/assets/1a38a252-13c3-49d1-a3f4-b21772f00c95" />


## Features

- handles basic HTTP/1.0 requests
- serves static files
- multi-threaded
- responds mostly
- thats about it

## Usage

```
# http-server -h
Usage: http-server [-w web_root] [-p port] [-t threads] [-v level]
  -w web_root  Web root directory (default: ./public)
  -p port      Server port (default: 3030)
  -t threads   Thread Count (default: 4)
  -v           Verbosity level (default: 4) (0:NONE, 1:ERROR, 2:WARN, 3:INFO, 4:DEBUG)
  -h           Show this help
```

## Running

```bash
make run
```
test it on port 3030
```bash
curl -v http://localhost:3030/
```
## TODO
- [x] http/1.0
- [x] multi-threading
- [x] cache files
- [ ] logs on their own thing
- [ ] cache lru cleaning
- [ ] update cache on file update
- [ ] cache common responses
- [ ] cleanup the code
- [ ] http/1.1
