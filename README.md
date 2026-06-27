# NGINX HTTP Early Hints (HTTP 103)

Allow nginx to generate its own 103 Early Hints response, when the official
release only supports forwarding them from upstreams.

Requires clients to send "Sec-Fetch-Dest: document" header, to avoid the risk
of misconfigurations accidentally sending 103 on subresources.

## Requirements

* nginx >= 1.29.0 (with 103 Early Hints plumbing)

## License

* BSD-2 to match upstream nginx (for easy conversion to a PR)

## Building (dynamic module)

As a dynamic module:

```sh
./configure --with-compat --add-dynamic-module=/path/to/ngx_http_early_hints
make modules
# copy objs/ngx_http_early_hints_module.so into your modules directory
```

Then load it:

```nginx
load_module modules/ngx_http_early_hints_module.so;
```

## Building (static module)

```sh
./configure --add-module=/path/to/ngx_http_early_hints
```

## Usage

```nginx
http {
    server {
        early_hints on;

        location / {
            # Add the resources you want to early hint
            early_hints_link "</css/app.css>; rel=preload; as=style";
            early_hints_link "</js/app.js>; rel=preload; as=script";

            fastcgi_pass unix:/run/php.sock;
        }
    }
}
```
