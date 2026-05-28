# Composition Mode Design

## Composition Mode

**mode** indicates how users are recorded:

- **Individual** - Each specified user is recorded separately into their own file. Other users joining the channel are ignored.
- **Composite** - All specified users (or all users in the channel if none specified) are composited into a single video with a chosen layout.

## Target Users

**targetUsers** specifies which users will be recorded.

- If a list of user IDs is provided, only those users are recorded.
- If empty (`[]`), all users currently in the channel are recorded (composite-all mode).
- For Individual mode, each listed user gets their own recording file.
- For Composite mode, all listed users are rendered into a single output.

## Layouts

**layout** controls how user video streams are arranged in composite mode. Only applies when mode is Composite.

### flat (default)

All users are arranged in equal-sized tiles that fill the canvas. The system automatically calculates the optimal grid dimensions (cols x rows) based on the number of active users and the canvas aspect ratio, preferring cells close to 16:9.

```
+-------+-------+        +---+---+---+
|       |       |        |   |   |   |
| user1 | user2 |        | 1 | 2 | 3 |
|       |       |        |   |   |   |
+-------+-------+        +---+---+---+
                          |   |   |   |
  2 users: 2x1            | 4 | 5 | 6 |
                          |   |   |   |
                          +---+---+---+

                            6 users: 3x2
```

- 1 user: full screen
- 2 users: side by side (landscape) or stacked (portrait)
- 3-4 users: 2x2
- 5-6 users: 3x2
- 7-9 users: 3x3
- Scales up to 6x4 (24 users max)

### spotlight

One user is displayed prominently in a large view, while the remaining users are shown in smaller equal-sized thumbnails along the edge.

```
+-------------------+-------+
|                   | user2 |
|                   +-------+
|     user1         | user3 |
|   (spotlight)     +-------+
|                   | user4 |
+-------------------+-------+
```

- The first user in the list (or the first to join) gets the large view.
- Other users are arranged as small tiles on the side.
- Useful for presentations, lectures, or any scenario with a primary speaker.

> Note: Spotlight layout is currently defined and accepted by the API but renders the same as flat in the native recorder. Full spotlight rendering is planned for a future release.

### customized (native recorder)

The API caller specifies exact bounding boxes (x, y, width, height, z-order) for each user's video on the canvas. This gives full control over positioning without requiring a web recorder.

```
+----------------------------------+
|  +--------+                      |
|  | user1  |    +-----------+     |
|  | (z:2)  |    |  user2    |     |
|  +--------+    |  (z:1)    |     |
|                +-----------+     |
|       +------+                   |
|       |user3 |                   |
|       |(z:0) |                   |
|       +------+                   |
+----------------------------------+
```

- Each user has an explicit bounding box: `{x, y, width, height, z}`.
- Z-order controls layering when bounding boxes overlap.
- Runs on the native C++ recorder (not the web recorder).
- Useful for picture-in-picture, custom overlays, or branded layouts.

> Note: Customized layout is accepted by the API but bounding box rendering is not yet implemented in the native recorder. Currently renders as flat.

### freestyle (web recorder only)

Routes the task to the web recorder engine (ModeWeb) instead of the native C++ worker. A custom HTML canvas URL (`freestyleCanvasUrl`) controls the layout rendering entirely via a web page.

```
+---------------------------------+
|                                 |
|   Custom HTML/CSS/JS canvas     |
|   (rendered by web recorder)    |
|                                 |
+---------------------------------+
```

- Requires `freestyleCanvasUrl` in the payload pointing to a hosted canvas page.
- The web recorder loads the canvas URL and captures the rendered output.
- Supports arbitrary layouts, overlays, animations, and branding.
- Tasks with freestyle layout are routed to a separate queue: `egress:web:*`.

## API Payload Examples

### Flat composite (2 users)
```json
{
  "cmd": "record",
  "payload": {
    "channel": "my-channel",
    "access_token": "...",
    "users": ["1001", "1002"],
    "layout": "flat"
  }
}
```

### Spotlight composite
```json
{
  "cmd": "record",
  "payload": {
    "channel": "my-channel",
    "access_token": "...",
    "users": ["speaker", "viewer1", "viewer2"],
    "layout": "spotlight"
  }
}
```

### Customized (bounding boxes)
```json
{
  "cmd": "record",
  "payload": {
    "channel": "my-channel",
    "access_token": "...",
    "users": ["1001", "1002", "1003"],
    "layout": "customized",
    "regions": [
      {"uid": "1001", "x": 0, "y": 0, "width": 640, "height": 480, "z": 2},
      {"uid": "1002", "x": 640, "y": 0, "width": 640, "height": 480, "z": 1},
      {"uid": "1003", "x": 480, "y": 360, "width": 320, "height": 240, "z": 3}
    ]
  }
}
```

### Freestyle (web recorder)
```json
{
  "cmd": "record",
  "payload": {
    "channel": "my-channel",
    "access_token": "...",
    "layout": "freestyle",
    "freestyleCanvasUrl": "https://example.com/my-custom-canvas",
    "web_recorder": { ... }
  }
}
```

### Composite-all (record everyone)
```json
{
  "cmd": "record",
  "payload": {
    "channel": "my-channel",
    "access_token": "...",
    "users": [],
    "layout": "flat"
  }
}
```

### Individual (single user)
```json
{
  "cmd": "record",
  "payload": {
    "channel": "my-channel",
    "access_token": "...",
    "users": ["1001"],
    "layout": "flat"
  }
}
```
