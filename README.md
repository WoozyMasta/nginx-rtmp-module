# NGINX-based Media Streaming Server

More docs in [wiki](https://github.com/WoozyMasta/nginx-rtmp-module/wiki/Directives)

## nginx-rtmp-module

> [!NOTE]
> This is a fork of
> [Roman Arutyunyan's nginx-rtmp-module](https://github.com/arut/nginx-rtmp-module).
>
> This fork adds dynamic multi-platform push, RTMPS/TLS outgoing relay,
> and `--without-http` build compatibility.  
> All credit for the original work goes to Roman Arutyunyan.

## Original project links

* Repo: <https://github.com/arut/nginx-rtmp-module>
* Project blog <http://nginx-rtmp.blogspot.com>
* Google group <https://groups.google.com/group/nginx-rtmp>
* Google group (Russian) <https://groups.google.com/group/nginx-rtmp-ru>
* Support the original author Roman Arutyunyan,
  the creator of nginx-rtmp-module: <http://arut.github.com/nginx-rtmp-module/>

### Features

* RTMP/RTMPS/HLS/MPEG-DASH live streaming
* RTMP/RTMPS Video on demand FLV/MP4, playing from local filesystem or HTTP
* Stream relay support for distributed streaming: push & pull models
* Recording streams in multiple FLVs
* H264/AAC support
* Online transcoding with FFmpeg
* HTTP callbacks (publish/play/record/update etc)
* Running external programs on certain events (exec)
* HTTP control module for recording audio/video and dropping clients
* Advanced buffering techniques to keep memory allocations at a minimum
  level for faster streaming and low memory footprint
* Proved to work with Wirecast, FMS, Wowza, JWPlayer, FlowPlayer,
  StrobeMediaPlayback, ffmpeg, avconv, rtmpdump, flvstreamer and many more
* Statistics in XML/XSL in machine- & human-readable form
* Linux/FreeBSD/MacOS/Windows

### Build

cd to NGINX source directory & run this:

```shell
./configure --add-module=/path/to/nginx-rtmp-module
make
make install
```

Several versions of nginx (1.3.14 - 1.5.0) require http_ssl_module to be
added as well:

```shell
./configure --add-module=/path/to/nginx-rtmp-module --with-http_ssl_module
```

For building debug version of nginx add `--with-debug`

```shell
./configure --add-module=/path/to-nginx/rtmp-module --with-debug
```

[Read more about debug log](https://github.com/WoozyMasta/nginx-rtmp-module/wiki/Debug-log)

### Windows limitations

Windows support is limited. These features are not supported

* execs
* static pulls
* auto_push

### RTMP URL format

```txt
rtmp://rtmp.example.com/app[/name]
```

* `app` - should match one of application {} blocks in config
* `name` - interpreted by each application, can be empty

### Multi-worker live streaming

Module supports multi-worker live
streaming through automatic stream pushing
to nginx workers. This option is toggled with
rtmp_auto_push directive.

### Example nginx.conf

```conf
rtmp {
    server {
        listen 1935;

        chunk_size 4000;

        # TV mode: one publisher, many subscribers
        application mytv {

            # enable live streaming
            live on;

            # record first 1K of stream
            record all;
            record_path /tmp/av;
            record_max_size 1K;

            # append current timestamp to each flv
            record_unique on;

            # publish only from localhost
            allow publish 127.0.0.1;
            deny publish all;

            #allow play all;
        }

        # Transcoding (ffmpeg needed)
        application big {
            live on;

            # On every pusblished stream run this command (ffmpeg)
            # with substitutions: $app/${app}, $name/${name} for application & stream name.
            #
            # This ffmpeg call receives stream from this application &
            # reduces the resolution down to 32x32. The stream is the published to
            # 'small' application (see below) under the same name.
            #
            # ffmpeg can do anything with the stream like video/audio
            # transcoding, resizing, altering container/codec params etc
            #
            # Multiple exec lines can be specified.

            exec ffmpeg -re -i rtmp://localhost:1935/$app/$name -vcodec flv -acodec copy -s 32x32
                        -f flv rtmp://localhost:1935/small/${name};
        }

        application small {
            live on;
            # Video with reduced resolution comes here from ffmpeg
        }

        application webcam {
            live on;

            # Stream from local webcam
            exec_static ffmpeg -f video4linux2 -i /dev/video0 -c:v libx264 -an
                                -f flv rtmp://localhost:1935/webcam/mystream;
        }

        application mypush {
            live on;

            # Every stream published here
            # is automatically pushed to
            # these two machines
            push rtmp1.example.com;
            push rtmp2.example.com:1934;
        }

        application mypull {
            live on;

            # Pull all streams from remote machine
            # and play locally
            pull rtmp://rtmp3.example.com pageUrl=www.example.com/index.html;
        }

        application mystaticpull {
            live on;

            # Static pull is started at nginx start
            pull rtmp://rtmp4.example.com pageUrl=www.example.com/index.html name=mystream static;
        }

        # video on demand
        application vod {
            play /var/flvs;
        }

        application vod2 {
            play /var/mp4s;
        }

        # Many publishers, many subscribers
        # no checks, no recording
        application videochat {

            live on;

            # The following notifications receive all
            # the session variables as well as
            # particular call arguments in HTTP POST
            # request

            # Make HTTP request & use HTTP retcode
            # to decide whether to allow publishing
            # from this connection or not
            on_publish http://localhost:8080/publish;

            # Same with playing
            on_play http://localhost:8080/play;

            # Publish/play end (repeats on disconnect)
            on_done http://localhost:8080/done;

            # All above mentioned notifications receive
            # standard connect() arguments as well as
            # play/publish ones. If any arguments are sent
            # with GET-style syntax to play & publish
            # these are also included.
            # Example URL:
            #   rtmp://localhost/myapp/mystream?a=b&c=d

            # record 10 video keyframes (no audio) every 2 minutes
            record keyframes;
            record_path /tmp/vc;
            record_max_frames 10;
            record_interval 2m;

            # Async notify about an flv recorded
            on_record_done http://localhost:8080/record_done;
        }

        # HLS

        # For HLS to work please create a directory in tmpfs (/tmp/hls here)
        # for the fragments. The directory contents is served via HTTP (see
        # http{} section in config)
        #
        # Incoming stream must be in H264/AAC. For iPhones use baseline H264
        # profile (see ffmpeg example).
        # This example creates RTMP stream from movie ready for HLS:
        #
        # ffmpeg -loglevel verbose -re -i movie.avi  -vcodec libx264
        #    -vprofile baseline -acodec libmp3lame -ar 44100 -ac 1
        #    -f flv rtmp://localhost:1935/hls/movie
        #
        # If you need to transcode live stream use 'exec' feature.
        #
        application hls {
            live on;
            hls on;
            hls_path /tmp/hls;
        }

        # MPEG-DASH is similar to HLS

        application dash {
            live on;
            dash on;
            dash_path /tmp/dash;
        }
    }
}

# HTTP can be used for accessing RTMP stats
http {
    server {
        listen      8080;

        # This URL provides RTMP statistics in XML
        location /stat {
            rtmp_stat all;

            # Use this stylesheet to view XML as web page
            # in browser
            rtmp_stat_stylesheet stat.xsl;
        }

        location /stat.xsl {
            # XML stylesheet to view RTMP stats.
            # Copy stat.xsl wherever you want
            # and put the full directory path here
            root /path/to/stat.xsl/;
        }

        location /hls {
            # Serve HLS fragments
            types {
                application/vnd.apple.mpegurl m3u8;
                video/mp2t ts;
            }
            root /tmp;
            add_header Cache-Control no-cache;
        }

        location /dash {
            # Serve DASH fragments
            root /tmp;
            add_header Cache-Control no-cache;
        }
    }
}
```

### Dynamic push (multi-platform restream)

Push a single stream to multiple platforms by passing stream keys as RTMP
query args. No ffmpeg, no shell, no HTTP callbacks required.

Client URL:

```txt
rtmp://your-server:1935/restream/live?yt=YOUTUBE_KEY&tw=TWITCH_KEY
```

nginx.conf:

```conf
rtmp {
    server {
        listen 1935;

        application restream {
            live on;
            record off;

            # IP whitelist - only allow your encoder
            allow publish 192.168.0.0/24;
            deny  publish all;
            deny  play    all;

            # Map query arg names to platform RTMP base URLs
            dynamic_push_arg yt    rtmp://a.rtmp.youtube.com/live2;
            dynamic_push_arg tw    rtmp://live.twitch.tv/app;
            dynamic_push_arg fb    rtmps://live-api-s.facebook.com:443/rtmp;

            # Reject publish if no known platform arg is present (optional)
            dynamic_push_required off;

            # Reject publish if any requested platform push fails (optional)
            dynamic_push_strict off;

            # Maximum stream key length accepted from client
            dynamic_push_key_max_len 256;
        }
    }
}
```

Note: certificate verification is disabled for outgoing relay - destination
URLs are fixed in the nginx config and controlled by the administrator.

Directives:

* `dynamic_push_arg <name> <rtmp://host/app> [nokey]` - map a query arg to a
  platform. The base URL must end at the application level (`rtmp://host/app`);
  the stream key comes from the client query arg. Multiple directives are
  allowed. With `nokey`, the arg value is ignored and the local stream name is
  used as the remote play path - useful for services that need no stream key:

  ```txt
      dynamic_push_arg local rtmp://127.0.0.1:1936/live nokey;
  ```

  Client activates it with any value: `?local=1`.
* `dynamic_push_required on|off` - when `on`, reject publish if no configured
  arg is present in the client URL. Default: `off`.
* `dynamic_push_strict on|off` - when `on`, reject publish if any requested
  platform push fails to connect. Default: `off`.
* `dynamic_push_key_max_len <bytes>` - maximum length of a stream key accepted
  from the client. Keys exceeding this or containing unsafe characters
  (`://`, `?`, `&`, `%`, whitespace, control chars) are rejected.
  Default: `512`.

Security: the client can only supply stream keys - it cannot override the
destination host. The upstream URLs are fixed in the nginx config.

### Multi-worker streaming example

```conf
rtmp_auto_push on;

rtmp {
    server {
        listen 1935;

        application mytv {
            live on;
        }
    }
}
```
