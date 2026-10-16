import QtQuick
import "MapLines.js" as MapLines

// Animated radar loop. WeatherBackend::fetchRadar() resolves one map tile
// (RainViewer's public API, no key) per available frame and loops them here —
// a real regional radar view without the app ever having to stitch a mosaic.
//
// The precip tile itself is a transparent-background PNG with no basemap
// (RainViewer doesn't offer one on the free tier, and a second raster tile
// service would mean a second network dependency, a possible API key, and a
// photographic/labelled look that clashes with this app's plain vector
// aesthetic). Instead, coastline/country/state outlines are drawn underneath
// it from a bundled dataset (MapLines.js, simplified Natural Earth
// 1:50m data, public domain), reprojected here at runtime, so there is
// always a map and never a network call for it.
//
// Under the rain sits a cloud layer: GOES-East infrared satellite frames
// (WeatherBackend::buildRadarLoop), one per radar frame, already converted to a
// transparent white overlay, so the stack bottom-up is black, clouds, rain,
// map lines, location marker.
Item {
    id: screen

    // One entry per frame, each a row-major grid of tile URLs, gridColumns
    // wide, north row first. The location's own tile is the middle column of
    // row centerRow (WeatherBackend::buildRadarLoop).
    property var frames: []
    property int gridColumns: 3
    property int centerRow: 0
    // Same shape as frames (time-matched); "" for any gap.
    property var cloudFrames: []
    property real markerX: 0.5
    property real markerY: 0.5
    // Tile identity, from WeatherBackend's own latLonToTile() — reused here so
    // MapLines' lon/lat data project into exactly the same tile-local space
    // markerX/markerY are already in, with the projection math written once.
    property int tileX: 0
    property int tileY: 0
    property int tileZoom: 6
    // Zoom asked for with up/down; differs from tileZoom while it downloads.
    property int requestedZoom: 6
    property int tileSourcePx: 512

    property int frameIndex: 0
    // Which of the two precip Image buffers is currently the visible one —
    // see the crossfade block below. Frame swaps fade between buffers rather
    // than hard-cutting the source on one Image, which used to read as a
    // flash every 500ms on a CRT (a lot of contrast change, no transition).
    property int visibleBuffer: 0

    function frameAt(i) { return i < frames.length ? frames[i] : [] }
    function cloudAt(i) { return i < cloudFrames.length ? cloudFrames[i] : [] }

    // Each buffer pair (imgA+cloudA, imgB+cloudB) holds one frame; which
    // frame is tracked on the radar strip. Buffer A shows frame 0, buffer B
    // preloads frame 1.
    onFramesChanged: {
        frameIndex = 0
        visibleBuffer = 0
        lastFlip = Date.now()
        loadBuffer(0, 0)
        loadBuffer(1, frames.length > 1 ? 1 : -1)
    }

    // The backend publishes both lists together, but Weather.qml assigns
    // frames first, so re-sync both cloud buffers to whatever frame their
    // radar partner holds rather than trusting the earlier assignment.
    onCloudFramesChanged: {
        cloudA.urls = imgA.frame >= 0 ? cloudAt(imgA.frame) : []
        cloudB.urls = imgB.frame >= 0 ? cloudAt(imgB.frame) : []
    }

    function loadBuffer(buffer, frame) {
        var r = buffer === 0 ? imgA : imgB
        var c = buffer === 0 ? cloudA : cloudB
        r.frame = frame
        r.urls = frame >= 0 ? frameAt(frame) : []
        c.urls = frame >= 0 ? cloudAt(frame) : []
    }

    // Time of the last buffer flip, for the Timer below.
    property real lastFlip: 0

    // Tiles are drawn at the box's height and laid out as a grid, placed so
    // the location sits at the exact center of the box at every zoom.
    // originX/originY are the top-left of the location's own tile; everything
    // that draws onto the map (tiles, lines, marker) goes through them.
    readonly property real tilePx: mapBox.height
    readonly property real originX: mapBox.width / 2 - markerX * tilePx
    readonly property real originY: mapBox.height / 2 - markerY * tilePx
    onOriginXChanged: mapCanvas.requestPaint()
    onOriginYChanged: mapCanvas.requestPaint()

    // The tile grid for one frame, the location's tile at originX/originY.
    // Used for both the precip and the cloud buffers.
    component TileStrip: Item {
        id: strip
        property var urls: []
        // Which frame this buffer holds, -1 for none (radar strips only).
        property int frame: -1
        // Every tile finished (an empty or failed one counts, so one bad tile
        // can't stall the loop forever).
        function isReady() {
            for (var i = 0; i < tiles.count; i++) {
                var t = tiles.itemAt(i)
                if (t && t.status === Image.Loading) return false
                if (!t && i < strip.urls.length) return false
            }
            return true
        }
        anchors.fill: parent
        Behavior on opacity { NumberAnimation { duration: 350; easing.type: Easing.InOutQuad } }
        Repeater {
            id: tiles
            model: strip.urls
            Image {
                required property var modelData
                required property int index
                x: screen.originX + (index % screen.gridColumns
                                     - Math.floor(screen.gridColumns / 2)) * screen.tilePx
                y: screen.originY + (Math.floor(index / screen.gridColumns)
                                     - screen.centerRow) * screen.tilePx
                width: screen.tilePx
                height: screen.tilePx
                source: modelData
                asynchronous: true
                cache: true
            }
        }
    }

    // The tile identity only changes when the module re-resolves a location
    // (rare — not per animation frame), so repainting the map lines on these
    // three alone, rather than on every markerX/markerY/frame tick, keeps the
    // Canvas cost to once per visit rather than twice a second.
    onTileXChanged: mapCanvas.requestPaint()
    onTileYChanged: mapCanvas.requestPaint()
    onTileZoomChanged: mapCanvas.requestPaint()

    // Standard Web Mercator tile projection (same formula as
    // WeatherBackend::latLonToTile) — a lon/lat's position in tile-fraction
    // units, 0..1 across tile (tileX, tileY) at the current zoom, continuing
    // past 0/1 for points in neighbouring tiles rather than clamping, so
    // lines that cross a tile edge still draw the segment that's in view.
    // Latitude is clamped to Mercator's valid range — a couple of points in
    // the bundled Antarctica ring sit at exactly -90, which is undefined for
    // this projection (tan/sec blow up); clamping keeps that ring's other,
    // valid points drawing correctly instead of the whole line silently going
    // NaN from one bad vertex.
    function fracFor(lon, lat) {
        var n = Math.pow(2, screen.tileZoom)
        var xf = (lon + 180.0) / 360.0 * n
        var clampedLat = Math.max(-85.05, Math.min(85.05, lat))
        var latRad = clampedLat * Math.PI / 180.0
        var yf = (1.0 - Math.log(Math.tan(latRad) + 1.0 / Math.cos(latRad)) / Math.PI) / 2.0 * n
        return [xf - screen.tileX, yf - screen.tileY]
    }

    // Tile-fraction (relative to the location's own tile) -> on-screen pixel
    // within mapBox, the same placement TileStrip uses, so the map lines, the
    // "you are here" marker, and the tiles always agree on where a lon/lat lands.
    function projectFrac(fracX, fracY) {
        return [screen.originX + fracX * screen.tilePx, screen.originY + fracY * screen.tilePx]
    }

    function hexToRgba(hex, alpha) {
        var h = hex.charAt(0) === "#" ? hex.substring(1) : hex
        var r = parseInt(h.substring(0, 2), 16) / 255
        var g = parseInt(h.substring(2, 4), 16) / 255
        var b = parseInt(h.substring(4, 6), 16) / 255
        return Qt.rgba(r, g, b, alpha)
    }

    // The radar is repainted in the backend (WeatherBackend recolorRadar):
    // rain in the standard NWS reflectivity colors, snow in its own blues.
    // One swatch per band, sampled from those same tables.
    readonly property var legendEntries: [
        { label: "LIGHT",    color: "#02fd02" },  // 20 dBZ
        { label: "MODERATE", color: "#008e00" },  // 30 dBZ
        { label: "HEAVY",    color: "#e5bc00" },  // 40 dBZ
        { label: "SEVERE",   color: "#fd0000" },  // 50 dBZ
        { label: "EXTREME",  color: "#f800fd" },  // 65 dBZ, hail
        { label: "SNOW",     color: "#7ab8ff" },  // 20 dBZ snow
        { label: "CLOUDS",   color: "#cccccc" }   // satellite layer, not radar
    ]

    Text {
        id: title
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        text: "RADAR"
        color: root.primaryColor
        font.family: root.globalFont
        font.pixelSize: root.sh * 0.05 //24
    }

    Item {
        id: tileArea
        anchors.top: title.bottom
        anchors.topMargin: root.sh * 0.0416667 //20
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: caption.top
        anchors.bottomMargin: root.sh * 0.0208333 //10

        // Full height, and wide enough to reach the legend. Capped at twice
        // the height, the most kMapColumns (3) tiles can always cover.
        Item {
            id: mapBox
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: Math.min(legend.x - root.sw * 0.025, height * 2)
            clip: true

            Rectangle {
                anchors.fill: parent
                color: "black"
            }

            // Cloud buffers, cross-faded in lockstep with the precip buffers
            // below so clouds and rain always show the same moment.
            TileStrip {
                id: cloudA
                opacity: screen.visibleBuffer === 0 ? 1 : 0
            }
            TileStrip {
                id: cloudB
                opacity: screen.visibleBuffer === 1 ? 1 : 0
            }

            // Two buffers cross-fading between precip frames — see
            // onFramesChanged/the Timer below for how they're driven.
            TileStrip {
                id: imgA
                opacity: screen.visibleBuffer === 0 ? 1 : 0
            }
            TileStrip {
                id: imgB
                opacity: screen.visibleBuffer === 1 ? 1 : 0
            }

            // Map lines on top of clouds and rain, like TV radar maps, so they
            // can't be buried under cloud cover.
            Canvas {
                id: mapCanvas
                anchors.fill: parent
                renderStrategy: Canvas.Cooperative

                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                // Repaint on a theme change, the lines take the theme's color.
                property color lineColor: root.primaryColor
                onLineColorChanged: requestPaint()

                onPaint: {
                    var ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    if (width <= 0 || height <= 0) return

                    // Cheap reject in plain lon/lat (no trig) before the real
                    // per-point projection: skip any line whose every point
                    // falls outside the location tile's lon/lat box, expanded
                    // to cover the whole tile grid plus half a tile, so lines
                    // crossing in from outside still draw their visible part.
                    var n = Math.pow(2, screen.tileZoom)
                    var lonMin = screen.tileX / n * 360 - 180
                    var lonMax = (screen.tileX + 1) / n * 360 - 180
                    function invMercLat(ty) {
                        var yy = Math.PI * (1 - 2 * ty / n)
                        return Math.atan(Math.sinh(yy)) * 180 / Math.PI
                    }
                    var latMax = invMercLat(screen.tileY)
                    var latMin = invMercLat(screen.tileY + 1)
                    var lonSpan = lonMax - lonMin, latSpan = latMax - latMin
                    var cols = Math.floor(screen.gridColumns / 2) + 0.5
                    var loLon = lonMin - lonSpan * cols, hiLon = lonMax + lonSpan * cols
                    var loLat = latMin - latSpan * 1.5, hiLat = latMax + latSpan * 1.5

                    // Every visible line goes into one path, stroked twice: a
                    // wider faint dark halo first, then the thin bright line,
                    // so borders stay readable over white cloud tops and dark
                    // clear sky alike (how TV radar maps draw them).
                    function addLines(lines) {
                        for (var i = 0; i < lines.length; i++) {
                            var pts = lines[i]
                            var visible = false
                            for (var k = 0; k < pts.length; k += 2) {
                                if (pts[k] >= loLon && pts[k] <= hiLon &&
                                    pts[k+1] >= loLat && pts[k+1] <= hiLat) {
                                    visible = true
                                    break
                                }
                            }
                            if (!visible) continue

                            for (var j = 0; j < pts.length; j += 2) {
                                var f = screen.fracFor(pts[j], pts[j+1])
                                var p = screen.projectFrac(f[0], f[1])
                                if (j === 0) ctx.moveTo(p[0], p[1])
                                else ctx.lineTo(p[0], p[1])
                            }
                        }
                    }

                    ctx.beginPath()
                    addLines(MapLines.stateLines)
                    addLines(MapLines.countryLines)
                    ctx.lineJoin = "round"
                    ctx.strokeStyle = Qt.rgba(0, 0, 0, 0.45)
                    ctx.lineWidth = 3
                    ctx.stroke()
                    ctx.strokeStyle = Qt.rgba(lineColor.r, lineColor.g, lineColor.b, 0.6)
                    ctx.lineWidth = 1
                    ctx.stroke()
                }
            }

            // "You are here" — the exact spot within the tile, from the same
            // lat/lon the rest of the module uses. Uses the same crop-aware
            // projection as the map lines and the precip images themselves.
            Rectangle {
                readonly property var pos: screen.projectFrac(screen.markerX, screen.markerY)
                width: root.sh * 0.0125 //6
                height: width
                radius: width / 2
                color: root.accentColor
                x: pos[0] - width / 2
                y: pos[1] - height / 2
            }

            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.color: root.tertiaryColor
                border.width: Math.max(1, root.sh * 0.003125) //2
            }

            // While a new zoom's tiles download, the old loop keeps playing
            // with this over it.
            Rectangle {
                visible: screen.requestedZoom !== screen.tileZoom
                         && screen.frames.length > 0
                anchors.centerIn: parent
                width: zoomingText.width + root.sw * 0.0333333 //24
                height: zoomingText.height + root.sh * 0.0166667 //8
                color: "black"
                opacity: 0.8
                Text {
                    id: zoomingText
                    anchors.centerIn: parent
                    text: "ZOOMING..."
                    color: root.primaryColor
                    font.family: root.globalFont
                    font.pixelSize: root.sh * 0.0333333 //16
                }
            }

            Text {
                visible: screen.frames.length === 0
                anchors.centerIn: parent
                text: "RADAR UNAVAILABLE"
                color: root.tertiaryColor
                font.family: root.globalFont
                font.pixelSize: root.sh * 0.0333333 //16
            }
        }

        // Legend — what the precip colors mean. Added alongside reverting the
        // tile to a square (rather than stretched edge-to-edge): a labelled
        // key needs somewhere to live, and the width a square frees up is
        // exactly that somewhere.
        Column {
            id: legend
            visible: screen.frames.length > 0
            anchors.right: parent.right
            anchors.verticalCenter: mapBox.verticalCenter
            spacing: root.sh * 0.0208333 //10

            Repeater {
                model: screen.legendEntries

                Row {
                    spacing: root.sw * 0.0138889 //10
                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: root.sh * 0.0375 //18
                        height: width
                        color: modelData.color
                        border.color: root.tertiaryColor
                        border.width: 1
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.label
                        color: root.tertiaryColor
                        font.family: root.globalFont
                        font.pixelSize: root.sh * 0.0416667 //20
                    }
                }
            }
        }
    }

    // RainViewer asks free/personal use to credit them with a link — this is
    // the on-screen equivalent since the app has no "website" to put one on.
    Text {
        id: caption
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        text: "RADAR: RAINVIEWER.COM · CLOUDS: NOAA GOES VIA NASA GIBS · MAP: NATURAL EARTH"
        color: root.tertiaryColor
        font.family: root.globalFont
        font.pixelSize: root.sh * 0.0208333 //10
    }

    // Advances the loop only once the next frame's radar AND cloud tiles have
    // both finished loading, so the two layers can never show different
    // moments. Ticks fast and decides by elapsed time: the hidden buffer gets
    // the next frame only after the outgoing fade (350ms) is done, so a
    // fading-out frame is never swapped mid-fade, and a flip happens no sooner
    // than 500ms after the last one.
    Timer {
        interval: 100
        running: screen.frames.length > 1
        repeat: true
        onTriggered: {
            var since = Date.now() - screen.lastFlip
            var next = (screen.frameIndex + 1) % screen.frames.length
            var hidden = screen.visibleBuffer === 0 ? 1 : 0
            var r = hidden === 0 ? imgA : imgB
            var c = hidden === 0 ? cloudA : cloudB
            if (since >= 400 && r.frame !== next)
                screen.loadBuffer(hidden, next)
            if (since >= 500 && r.frame === next && r.isReady() && c.isReady()) {
                screen.frameIndex = next
                screen.visibleBuffer = hidden
                screen.lastFlip = Date.now()
            }
        }
    }
}
