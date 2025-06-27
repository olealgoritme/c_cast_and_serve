# Cast and Serve

Serves local video files over HTTP and automatically casts them to Chromecast devices.

## Protocol Implementation

- **Discovery**: mDNS/Avahi to find `_googlecast._tcp` services
- **HTTP Server**: Boost.Beast serving video files with proper MIME types  
- **Cast V2 Protocol**: TLS connection to port 8009 with protobuf messages
  - Length-prefixed `CastMessage` protobuf frames (not WebSocket/JSON)
  - Namespace routing: connection → receiver → media
  - Session flow: CONNECT → LAUNCH app → CONNECT to app → LOAD media
  - Sends `contentId`, `contentType`, `streamType`, `autoplay` in LOAD

## Usage

```bash
./cast_and_serve <video_file> <port>
./cast_and_serve ~/Videos/movie.mp4 8080
```
