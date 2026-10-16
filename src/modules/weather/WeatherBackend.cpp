#include "WeatherBackend.h"
#include "RadarPalette.h"
#include "../../util/MpvLocator.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalSocket>
#include <QProcess>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QDate>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QPointer>
#include <QRegularExpression>
#include <QLocale>
#include <QTimeZone>
#include <QtMath>
#include <QDebug>
#include <algorithm>
#include <cmath>
#include <memory>

namespace {

const char *kModuleId   = "com.240mp.weather";
const char *kForecastUrl = "https://api.open-meteo.com/v1/forecast";
const char *kGeocodeUrl  = "https://geocoding-api.open-meteo.com/v1/search";
const char *kNwsPointsUrl = "https://api.weather.gov/points";

// RainViewer's public radar-frame list — free, no API key, ~1200 radars
// worldwide, refreshed every 10 minutes. No formal SLA (see their API docs),
// hence fetchRadar() treating any failure as non-fatal.
const char *kRadarMapsUrl = "https://api.rainviewer.com/public/weather-maps.json";
// Single tile at this zoom (RainViewer's max is 7) covers a wide-enough region
// to see weather approaching without stitching a mosaic — a many-tile grid
// would multiply the request count by the frame count for little benefit on a
// CRT-scale display. "2" is RainViewer's "Universal Blue" colour scheme.
// Default zoom, and the range up/down on the radar screen can step through.
// RainViewer serves 512px tiles up to zoom 7 (8 answers with a placeholder),
// so zoom 8 crops and upscales the zoom-7 parent tile, the same trick the
// clouds use past kCloudMaxZoom. A zoom-7 512px tile is already finer than
// the radar data itself, so the upscale costs little.
constexpr int kRadarDefaultZoom = 6;
constexpr int kRadarMinZoom = 3;
constexpr int kRadarMaxZoom = 8;
constexpr int kRadarSourceMaxZoom = 7;
constexpr int kRadarTileSize = 512;
const char *kRadarColorScheme = "2";
// Smoothing off so every pixel is an exact entry of the color table (smoothed
// tiles blend colors that no longer decode); snow in its own colors on.
const char *kRadarTileOptions = "0_1";
// Tiles fetched per frame, side by side west to east around the location's own
// tile, so the screen can show a map wider than it is tall. Three always covers
// a box up to twice as wide as tall wherever the location sits in its tile.
constexpr int kMapColumns = 3;
// Rows likewise: the location's own tile row plus whichever neighbour row is
// nearer the location, which is always enough to center the location
// vertically in a box one tile tall.
constexpr int kMapRows = 2;

// Tile x for column `col` (0..kMapColumns-1) around center tile x, wrapping at
// the antimeridian.
int columnTileX(int centerX, int col, int zoom) {
    const int n = 1 << zoom;
    return ((centerX + col - kMapColumns / 2) % n + n) % n;
}

// Cloud cover: GOES-East ABI band 13 ("clean" longwave infrared) from NASA
// GIBS, free and keyless, a new frame every 10 minutes, day and night alike
// (a visible-light band would go black after sunset). Served as Web Mercator
// XYZ tiles at the same zoom as the radar, so the same tile x/y covers the
// same ground. kCloudDomainsUrl answers which times exist, so frames are only
// ever requested for timestamps GIBS actually has. GIBS stops at zoom 6
// (kCloudMaxZoom); deeper radar zooms crop and upscale the zoom-6 parent tile,
// which is about the satellite's own ~2km resolution anyway.
const char *kCloudLayer = "GOES-East_ABI_Band13_Clean_Infrared";
const char *kCloudTileUrl =
    "https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/%1/default/%2/GoogleMapsCompatible_Level6/%3/%4/%5.png";
const char *kCloudDomainsUrl =
    "https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/1.0.0/%1/default/GoogleMapsCompatible_Level6/all/%2--%3.xml";
constexpr qint64 kCloudStepSecs = 600;
constexpr int    kCloudMaxZoom = 6;
// The tiles are opaque grayscale, brighter = colder. Warm, clear ground and sea
// land around gray 85-110, cloud tops above ~120 (measured off live tiles over
// the US east coast). Gray is ramped to alpha between these two, so clear
// ground drops out entirely and the map lines show through it.
constexpr int    kCloudGrayClear = 115;
constexpr int    kCloudGrayFull  = 195;
constexpr double kCloudMaxAlpha  = 0.85;

// RainViewer's free API only serves its "Universal Blue" scheme (other color
// IDs come back byte-identical), whose rain and snow blues look alike and
// whose drizzle is tan. Each pixel is decoded back to rain/snow + dBZ through
// the published table (RadarPalette.h) and repainted: rain in the standard
// NWS reflectivity colors (MetPy's NWSReflectivity table), snow in its own
// pale-to-deep blue scale so it never reads as rain.
struct RadarStep { int dbz; QRgb color; };
const RadarStep kNwsRain[] = {
    {10, qRgba(0x02, 0xfd, 0x02, 140)},   // very light: faded green
    {20, qRgb(0x02, 0xfd, 0x02)}, {25, qRgb(0x01, 0xc5, 0x01)}, {30, qRgb(0x00, 0x8e, 0x00)},
    {35, qRgb(0xfd, 0xf8, 0x02)}, {40, qRgb(0xe5, 0xbc, 0x00)}, {45, qRgb(0xfd, 0x95, 0x00)},
    {50, qRgb(0xfd, 0x00, 0x00)}, {55, qRgb(0xd4, 0x00, 0x00)}, {60, qRgb(0xbc, 0x00, 0x00)},
    {65, qRgb(0xf8, 0x00, 0xfd)}, {70, qRgb(0x98, 0x54, 0xc6)}, {75, qRgb(0xfd, 0xfd, 0xfd)},
};
const RadarStep kSnow[] = {
    {5,  qRgba(0xd6, 0xec, 0xff, 170)},   // flurries
    {15, qRgb(0xa8, 0xd4, 0xff)}, {20, qRgb(0x7a, 0xb8, 0xff)}, {25, qRgb(0x4f, 0x9a, 0xff)},
    {30, qRgb(0x2f, 0x78, 0xf0)}, {35, qRgb(0x1c, 0x55, 0xd6)},
};

template <size_t N>
QRgb radarStepColor(const RadarStep (&steps)[N], int dbz) {
    QRgb c = 0;   // below the first step: transparent (drizzle, noise)
    for (const RadarStep &s : steps)
        if (dbz >= s.dbz) c = s.color;
    return c;
}

QImage recolorRadar(const QImage &tile) {
    // Universal Blue color -> repainted color, built once.
    static const QHash<QRgb, QRgb> lookup = [] {
        QHash<QRgb, QRgb> map;
        const auto add = [&map](const uint32_t *table, bool snow) {
            for (int i = 0; i < kRadarPaletteSize; ++i) {
                const uint32_t v = table[i];
                if ((v & 0xff) == 0) continue;   // fully transparent
                const QRgb key = qRgba(v >> 24, (v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
                const int dbz = kRadarPaletteFirstDbz + i;
                if (!map.contains(key))
                    map.insert(key, snow ? radarStepColor(kSnow, dbz) : radarStepColor(kNwsRain, dbz));
            }
        };
        add(kUniversalBlueRain, false);
        add(kUniversalBlueSnow, true);
        return map;
    }();

    const QImage src = tile.convertToFormat(QImage::Format_ARGB32);
    QImage out(src.size(), QImage::Format_ARGB32);
    for (int y = 0; y < src.height(); ++y) {
        const QRgb *in = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        QRgb *o = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < src.width(); ++x)
            o[x] = qAlpha(in[x]) == 0 ? 0 : lookup.value(in[x], 0);
    }
    return out;
}

// Brightness temperature (as gray) to a white overlay with matching alpha.
QImage cloudOverlayFromInfrared(const QImage &infrared) {
    const QImage src = infrared.convertToFormat(QImage::Format_ARGB32);
    QImage out(src.size(), QImage::Format_ARGB32);
    for (int y = 0; y < src.height(); ++y) {
        const QRgb *in = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        QRgb *o = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            const double t = qBound(0.0, double(qGray(in[x]) - kCloudGrayClear)
                                             / (kCloudGrayFull - kCloudGrayClear), 1.0);
            o[x] = qRgba(255, 255, 255, int(255 * kCloudMaxAlpha * t));
        }
    }
    return out;
}

// api.weather.gov answers 403 to requests without a User-Agent, and asks for
// contact information in it. Qt sends none by default.
const char *kNwsUserAgent = "240-MP/" APP_VERSION " (https://github.com/anthonycaccese/240-MP)";

// Open-Meteo publishes data roughly every 15 minutes.
constexpr int kRefreshMs = 10 * 60 * 1000;

// Played when the user has no weather_music.txt, so the module sounds right
// with zero setup. Written the way a user would write them — spaces and all —
// because musicPlaylist() applies the same encoding rule to both sources.
//
// The archive.org collection carries every track twice, .mp3 and .ogg; these
// are the mp3s.
const char *kBuiltInMusic[] = {
    "https://archive.org/download/weatherscancompletecollection/01 Fair Weather.mp3",
    "https://archive.org/download/weatherscancompletecollection/01 Lazy Days.mp3",
    "https://archive.org/download/weatherscancompletecollection/02 Beach Frolic.mp3",
    "https://archive.org/download/weatherscancompletecollection/03 Winter Tundra.mp3",
    "https://archive.org/download/weatherscancompletecollection/04 Rainy Days.mp3",
    "https://archive.org/download/weatherscancompletecollection/05 Easy Times.mp3",
    "https://archive.org/download/weatherscancompletecollection/05 Midnight Cruise.mp3",
    "https://archive.org/download/weatherscancompletecollection/06 Tropical Breeze.mp3",
};

// A station further away than this is describing somewhere else's weather.
constexpr double kMaxStationKm = 25.0;
// Tried in order when one is unreachable or reporting nothing current.
constexpr int    kMaxStations  = 3;
// METARs are issued hourly at :50, with specials in between — but stations also
// drop off for days at a time, and a day-old observation is worse than the
// model. One cycle plus a margin.
constexpr qint64 kObsMaxAgeSec = 75 * 60;

// Private extension to WMO 4677, which has no code for a broken deck: it offers
// four sky states (0–3) where METAR reports five. Codes at 100+ are ours, are
// never emitted by Open-Meteo, and share the domain so that anything mapping
// from codes — the planned icons, most of all — needs only one lookup table.
constexpr int kCodeMostlyCloudy = 103;

// WMO 4677 weather interpretation codes, as short uppercase strings.
//
// Note these merge two things a METAR keeps separate: sky condition (CLEAR,
// OVERCAST) and present weather (RAIN, FOG, THUNDERSTORM). One code, so one
// condition line — which is what the WeatherStar layout wants anyway.
QString conditionForCode(int code) {
    switch (code) {
    case 0:  return QStringLiteral("CLEAR");
    case 1:  return QStringLiteral("MAINLY CLEAR");
    case 2:  return QStringLiteral("PARTLY CLOUDY");
    case 3:  return QStringLiteral("OVERCAST");
    case 45: case 48:            return QStringLiteral("FOG");
    case 51: case 53: case 55:   return QStringLiteral("DRIZZLE");
    case 56: case 57:            return QStringLiteral("FREEZING DRIZZLE");
    case 61: case 63: case 65:   return QStringLiteral("RAIN");
    case 66: case 67:            return QStringLiteral("FREEZING RAIN");
    case 71: case 73: case 75:   return QStringLiteral("SNOW");
    case 77:                     return QStringLiteral("SNOW GRAINS");
    case 80: case 81: case 82:   return QStringLiteral("SHOWERS");
    case 85: case 86:            return QStringLiteral("SNOW SHOWERS");
    case 79:                     return QStringLiteral("SLEET");
    case 89:                     return QStringLiteral("HAIL");
    case 95:                     return QStringLiteral("THUNDERSTORM");
    case 96: case 99:            return QStringLiteral("THUNDERSTORM HAIL");
    case kCodeMostlyCloudy:      return QStringLiteral("MOSTLY CLOUDY");
    default: break;
    }
    return QStringLiteral("UNKNOWN");
}

// Short forms for the Extended Forecast columns, which are a third of the screen
// wide. Long single words are the problem — WordWrap can't break "THUNDERSTORM",
// so it runs into the next column. The original had the same split: full wording
// on Current Conditions, abbreviations across the three-day columns.
//
// These also read as a *forecast* rather than an observation: a clear day ahead
// is SUNNY, whereas conditions right now are CLEAR. Hence isDay — the Extended
// Forecast columns always pass true, because a forecast for a whole day is a
// daytime characterisation by definition, but Other Locations reuses these
// short forms for *current* conditions in a narrow column and would otherwise
// call a clear sky SUNNY at three in the morning.
QString conditionShortForCode(int code, bool isDay) {
    switch (code) {
    case 0:                      return isDay ? QStringLiteral("SUNNY")
                                              : QStringLiteral("CLEAR");
    case 1:                      return isDay ? QStringLiteral("SUNNY")
                                              : QStringLiteral("MAINLY CLEAR");
    case 2:                      return QStringLiteral("PARTLY CLOUDY");  // wraps to two lines
    case 3:                      return QStringLiteral("CLOUDY");
    case 45: case 48:            return QStringLiteral("FOG");
    case 51: case 53: case 55:   return QStringLiteral("DRIZZLE");
    case 56: case 57:            return QStringLiteral("FRZ DRIZZLE");
    case 61: case 63: case 65:   return QStringLiteral("RAIN");
    case 66: case 67:            return QStringLiteral("FRZ RAIN");
    case 71: case 73: case 75:   return QStringLiteral("SNOW");
    case 77:                     return QStringLiteral("SNOW");
    case 80: case 81: case 82:   return QStringLiteral("SHOWERS");
    case 85: case 86:            return QStringLiteral("SNOW SHOWERS");
    case 79:                     return QStringLiteral("SLEET");
    case 89:                     return QStringLiteral("HAIL");
    case 95: case 96: case 99:   return QStringLiteral("T'STORMS");
    // Only reachable if a station code ever reaches these columns; the forecast
    // is Open-Meteo's, which never emits the private band.
    case kCodeMostlyCloudy:      return QStringLiteral("CLOUDY");
    default: break;
    }
    return QStringLiteral("UNKNOWN");
}

// Asset name in modules/weather/assets/images/wx/, without the .svg.
//
// Named by what the icon depicts rather than by code, because the icon set is
// far smaller than the code set — 61/63/65 are one rain picture, and three
// files named for the codes would be three identical assets that drift apart
// the first time one is edited.
//
// Only the four sky states take -day/-night variants: they are the ones with a
// sun or moon in them. Rain looks the same at midnight.
//
// Returns empty for anything unmapped, which the views treat as "draw no icon"
// rather than rendering a broken image.
QString iconForCode(int code, bool isDay) {
    const auto sky = [isDay](const char *base) {
        return QString::fromLatin1(base) + (isDay ? QStringLiteral("-day")
                                                  : QStringLiteral("-night"));
    };
    switch (code) {
    case 0:                      return sky("clear");
    case 1:                      return sky("mainly-clear");
    case 2:                      return sky("partly-cloudy");
    case kCodeMostlyCloudy:      return sky("mostly-cloudy");
    case 3:                      return QStringLiteral("overcast");
    case 45: case 48:            return QStringLiteral("fog");
    case 51: case 53: case 55:   return QStringLiteral("drizzle");
    case 56: case 57:            return QStringLiteral("freezing-drizzle");
    case 61: case 63: case 65:   return QStringLiteral("rain");
    case 66: case 67:            return QStringLiteral("freezing-rain");
    case 71: case 73: case 75:   return QStringLiteral("snow");
    case 77:                     return QStringLiteral("snow");
    case 79:                     return QStringLiteral("sleet");
    case 80: case 81: case 82:   return QStringLiteral("showers");
    case 85: case 86:            return QStringLiteral("snow-showers");
    case 89:                     return QStringLiteral("hail");
    case 95:                     return QStringLiteral("thunderstorm");
    case 96: case 99:            return QStringLiteral("thunderstorm-hail");
    default: break;
    }
    return {};
}

// ── METAR → code ─────────────────────────────────────────────────────────────
//
// A station reports sky condition and present weather as two independent
// fields, which is the distinction WMO 4677 throws away. Collapsing them here
// rather than in the view keeps one code domain for both data sources.

// Cloud amount ordered by how much sky is covered, so the worst layer wins.
// Plain code order won't do it: the private 103 sorts above 3 numerically but
// sits below OVERCAST in coverage.
int skyRank(int code) {
    switch (code) {
    case 0:                 return 0;
    case 1:                 return 1;
    case 2:                 return 2;
    case kCodeMostlyCloudy: return 3;
    case 3:                 return 4;
    default:                return -1;
    }
}

int codeForSky(const QJsonArray &layers) {
    // No layers at all is a clear sky: METAR omits the group rather than
    // reporting zero coverage.
    int best = 0;
    for (const QJsonValue &v : layers) {
        const QString amount = v.toObject()[QStringLiteral("amount")].toString();
        int code = -1;
        if      (amount == QLatin1String("CLR") || amount == QLatin1String("SKC")) code = 0;
        else if (amount == QLatin1String("FEW")) code = 1;
        else if (amount == QLatin1String("SCT")) code = 2;
        else if (amount == QLatin1String("BKN")) code = kCodeMostlyCloudy;
        // VV is vertical visibility — the sky is obscured, which from the
        // ground looks like and is reported as full cover.
        else if (amount == QLatin1String("OVC") || amount == QLatin1String("VV")) code = 3;
        if (skyRank(code) > skyRank(best)) best = code;
    }
    return best;
}

// null intensity is moderate — METAR marks only the extremes (-RA, +RA).
int byIntensity(const QString &intensity, int light, int moderate, int heavy) {
    if (intensity == QLatin1String("light")) return light;
    if (intensity == QLatin1String("heavy")) return heavy;
    return moderate;
}

// Present weather → code, or -1 when nothing in the group is worth showing and
// the caller should fall back to sky cover.
//
// Note that showers and freezing are `modifier` values rather than distinct
// `weather` values (SHRA is rain + showers, FZRA is rain + freezing), and that
// `inVicinity` marks weather near the field but not at it — VC in the raw
// METAR — which shouldn't be reported as the current condition.
int codeForPresentWeather(const QJsonArray &pw, double visMetres, bool haveVis) {
    int best = -1;
    int bestPriority = -1;

    // A group may list several phenomena (RA BR). Report the most consequential
    // rather than whichever the station happened to encode first.
    const auto consider = [&](int code, int priority) {
        if (code >= 0 && priority > bestPriority) { bestPriority = priority; best = code; }
    };

    bool hasHail = false;
    for (const QJsonValue &v : pw) {
        const QString w = v.toObject()[QStringLiteral("weather")].toString();
        if (w == QLatin1String("hail") || w == QLatin1String("snow_pellets")) hasHail = true;
    }

    for (const QJsonValue &v : pw) {
        const QJsonObject o = v.toObject();
        if (o[QStringLiteral("inVicinity")].toBool()) continue;

        const QString w   = o[QStringLiteral("weather")].toString();
        const QString in  = o[QStringLiteral("intensity")].toString();
        const QString mod = o[QStringLiteral("modifier")].toString();
        const bool freezing = (mod == QLatin1String("freezing"));
        const bool showers  = (mod == QLatin1String("showers"));

        if (w == QLatin1String("thunderstorms")) {
            consider(hasHail ? 96 : 95, 5);
        } else if (w == QLatin1String("drizzle")) {
            consider(freezing ? byIntensity(in, 56, 57, 57)
                              : byIntensity(in, 51, 53, 55),
                     freezing ? 4 : 2);
        } else if (w == QLatin1String("rain")) {
            if (freezing)     consider(byIntensity(in, 66, 67, 67), 4);
            else if (showers) consider(byIntensity(in, 80, 81, 82), 2);
            else              consider(byIntensity(in, 61, 63, 65), 2);
        } else if (w == QLatin1String("snow")) {
            consider(showers ? byIntensity(in, 85, 86, 86)
                             : byIntensity(in, 71, 73, 75), 3);
        } else if (w == QLatin1String("snow_grains")) {
            consider(77, 3);
        } else if (w == QLatin1String("ice_pellets")) {
            consider(79, 3);
        } else if (w == QLatin1String("hail") || w == QLatin1String("snow_pellets")) {
            consider(89, 3);
        } else if (w == QLatin1String("fog")) {
            consider(freezing ? 48 : 45, 1);
        } else if (w == QLatin1String("fog_mist")) {
            // BR covers everything from thin haze to near-zero visibility, and
            // automated stations report it in any humid air. Calling FOG on a
            // muggy evening with ten miles of visibility would be its own
            // version of the bug this exists to fix, so require the visibility
            // to actually be down before it outranks the sky.
            if (haveVis && visMetres < 3000.0) consider(45, 1);
        }
        // Everything else (haze, smoke, dust, squalls, funnel_cloud…) has no
        // WMO code this module displays, and defers to the sky condition.
    }
    return best;
}

// Great-circle distance in km. Used only to reject stations reporting from too
// far away, so the spherical approximation is far more precision than needed.
double distanceKm(double lat1, double lon1, double lat2, double lon2) {
    constexpr double R = 6371.0;
    const double dLat = qDegreesToRadians(lat2 - lat1);
    const double dLon = qDegreesToRadians(lon2 - lon1);
    const double a = qSin(dLat / 2) * qSin(dLat / 2)
                   + qCos(qDegreesToRadians(lat1)) * qCos(qDegreesToRadians(lat2))
                       * qSin(dLon / 2) * qSin(dLon / 2);
    return 2 * R * qAtan2(qSqrt(a), qSqrt(1 - a));
}

// An observation's values, kept raw and metric — exactly as NWS sends them
// regardless of any parameter — so a units change re-formats without another
// request. Any field can be null on an otherwise good observation, so each is
// kept only when present and applied independently.
QVariantMap parseObservation(const QJsonObject &props) {
    const auto value = [&props](const char *key) -> QVariant {
        const QJsonValue v = props[QLatin1String(key)].toObject()[QStringLiteral("value")];
        return v.isDouble() ? QVariant(v.toDouble()) : QVariant();
    };

    QVariantMap obs;
    obs["temperature"] = value("temperature");
    obs["dewPoint"]    = value("dewpoint");
    obs["humidity"]    = value("relativeHumidity");
    obs["windDir"]     = value("windDirection");
    obs["windSpeed"]   = value("windSpeed");
    obs["visibility"]  = value("visibility");
    // seaLevelPressure only, never barometricPressure: that one is station
    // pressure, which is a different quantity (tens of millibars out at
    // altitude) and would silently disagree with the model's value it replaces.
    // Null here just leaves Open-Meteo's pressure_msl in place.
    obs["pressure"]    = value("seaLevelPressure");

    const bool haveVis = obs["visibility"].isValid();
    const int  pwCode  = codeForPresentWeather(props[QStringLiteral("presentWeather")].toArray(),
                                               obs["visibility"].toDouble(), haveVis);
    obs["conditionCode"] = (pwCode >= 0)
        ? pwCode
        : codeForSky(props[QStringLiteral("cloudLayers")].toArray());
    return obs;
}

QNetworkRequest nwsRequest(const QUrl &url) {
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(kNwsUserAgent));
    return req;
}

// ── Location disambiguation ──────────────────────────────────────────────────
//
// Open-Meteo's geocoder has no country/region filter (countryCode, country_code
// and country are all silently ignored) and orders purely by population. So
// "Portland, ME, USA" returns Portland **Oregon** first, and picking the first
// result shows confidently wrong weather with no error anywhere — the worst
// failure this module can have. Everything below exists to prevent that.
//
// Results carry: name, admin1 (region, spelled out), country, country_code,
// population. A qualifier is matched against those in several ways because
// people write locations in several ways.

// Aliases people actually type for countries whose ISO code isn't obvious.
QString countryAlias(const QString &q) {
    static const QHash<QString, QString> kAliases = {
        { "USA", "US" }, { "U.S.A.", "US" }, { "U.S.", "US" }, { "AMERICA", "US" },
        { "UNITED STATES OF AMERICA", "US" },
        { "UK", "GB" }, { "BRITAIN", "GB" }, { "GREAT BRITAIN", "GB" },
        { "ENGLAND", "GB" }, { "SCOTLAND", "GB" }, { "WALES", "GB" },
        { "HOLLAND", "NL" }, { "UAE", "AE" }, { "SOUTH KOREA", "KR" },
    };
    return kAliases.value(q.toUpper());
}

// US states and DC. Needed because "City, ST" is the dominant way Americans
// write a location, and the abbreviations aren't derivable from the spelled-out
// name the API returns (TX from Texas, CA from California…).
QString expandUsState(const QString &q) {
    static const QHash<QString, QString> kStates = {
        {"AL","Alabama"},{"AK","Alaska"},{"AZ","Arizona"},{"AR","Arkansas"},
        {"CA","California"},{"CO","Colorado"},{"CT","Connecticut"},{"DE","Delaware"},
        {"DC","District of Columbia"},{"FL","Florida"},{"GA","Georgia"},{"HI","Hawaii"},
        {"ID","Idaho"},{"IL","Illinois"},{"IN","Indiana"},{"IA","Iowa"},
        {"KS","Kansas"},{"KY","Kentucky"},{"LA","Louisiana"},{"ME","Maine"},
        {"MD","Maryland"},{"MA","Massachusetts"},{"MI","Michigan"},{"MN","Minnesota"},
        {"MS","Mississippi"},{"MO","Missouri"},{"MT","Montana"},{"NE","Nebraska"},
        {"NV","Nevada"},{"NH","New Hampshire"},{"NJ","New Jersey"},{"NM","New Mexico"},
        {"NY","New York"},{"NC","North Carolina"},{"ND","North Dakota"},{"OH","Ohio"},
        {"OK","Oklahoma"},{"OR","Oregon"},{"PA","Pennsylvania"},{"RI","Rhode Island"},
        {"SC","South Carolina"},{"SD","South Dakota"},{"TN","Tennessee"},{"TX","Texas"},
        {"UT","Utah"},{"VT","Vermont"},{"VA","Virginia"},{"WA","Washington"},
        {"WV","West Virginia"},{"WI","Wisconsin"},{"WY","Wyoming"},
    };
    return kStates.value(q.toUpper());
}

// "New South Wales" -> "NSW". Free generalisation that covers multi-word regions
// worldwide (BC, NSW, NT…) without another table.
QString initialsOf(const QString &s) {
    QString out;
    const QStringList words = s.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    for (const QString &w : words)
        if (!w.isEmpty()) out += w.at(0).toUpper();
    return out;
}

bool qualifierMatches(const QJsonObject &r, const QString &qualifier) {
    const QString q       = qualifier.trimmed();
    if (q.isEmpty()) return true;
    const QString admin1  = r["admin1"].toString();
    const QString country = r["country"].toString();
    const QString code    = r["country_code"].toString();

    if (code.compare(q, Qt::CaseInsensitive) == 0)                       return true;
    if (countryAlias(q).compare(code, Qt::CaseInsensitive) == 0)         return true;
    if (admin1.compare(q, Qt::CaseInsensitive) == 0)                     return true;
    if (country.compare(q, Qt::CaseInsensitive) == 0)                    return true;
    if (expandUsState(q).compare(admin1, Qt::CaseInsensitive) == 0
            && !admin1.isEmpty())                                        return true;
    if (!admin1.isEmpty() && initialsOf(admin1).compare(q, Qt::CaseInsensitive) == 0)
                                                                         return true;
    // Substring only for longer qualifiers: the API returns "The Netherlands",
    // so an exact test fails for "Amsterdam, Netherlands" — but a substring test
    // on a 2-letter code would match "Paris, US" against "Australia".
    if (q.size() >= 4 && (admin1.contains(q, Qt::CaseInsensitive)
                          || country.contains(q, Qt::CaseInsensitive)))  return true;
    return false;
}

// 16-point compass, matching how the original displayed wind.
QString cardinal(double degrees) {
    static const char *points[] = {
        "N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
        "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"
    };
    int idx = int(qRound(degrees / 22.5)) % 16;
    if (idx < 0) idx += 16;
    return QString::fromLatin1(points[idx]);
}

} // namespace

WeatherBackend::WeatherBackend(const QString &appRoot, const QString &dataRoot,
                               QObject *parent)
    : QObject(parent)
    , m_appRoot(appRoot)
    , m_dataRoot(dataRoot)
    , m_nam(new QNetworkAccessManager(this))
    , m_refresh(new QTimer(this)) {
    m_refresh->setInterval(kRefreshMs);
    connect(m_refresh, &QTimer::timeout, this, [this]() {
        fetchWeather();
        fetchOthers();
        fetchObservation();
        fetchOtherObservations();
        fetchRadar();
    });

    // Its own socket name: MpvController uses /240mp-mpv.sock for the player,
    // and two mpvs sharing one path would hand the wrong process our commands.
    m_musicSocketPath = QDir::tempPath() + QStringLiteral("/240mp-weather-music.sock");
}

WeatherBackend::~WeatherBackend() {
    stopMusic();
}

QString WeatherBackend::location_file_path() const {
    return m_dataRoot + QStringLiteral("/weather_location.txt");
}

QString WeatherBackend::music_file_path() const {
    return m_dataRoot + QStringLiteral("/weather_music.txt");
}

QJsonObject WeatherBackend::loadConfig() const {
    QFile f(m_dataRoot + QStringLiteral("/config.json"));
    if (f.open(QIODevice::ReadOnly)) {
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (err.error == QJsonParseError::NoError && doc.isObject())
            return doc.object();
    }
    return {};
}

QJsonObject WeatherBackend::moduleConfig() const {
    return loadConfig()["modules"].toObject()[kModuleId].toObject();
}

bool WeatherBackend::useUsUnits() const {
    return moduleConfig()["units"].toString(QStringLiteral("Metric"))
               .compare(QLatin1String("US"), Qt::CaseInsensitive) == 0;
}

void WeatherBackend::onSettingChanged(const QString &moduleId, const QString &key,
                                      const QVariant &value) {
    Q_UNUSED(value)
    if (moduleId != QLatin1String(kModuleId)) return;
    // Units change the requested values, not just their presentation, so a
    // switch has to re-fetch rather than reformat. Both fetches: the extras are
    // a separate request, and refreshing only the primary left the Other
    // Locations table in the old units while its °C/°F heading — which reads
    // config live — had already flipped.
    if (key == QLatin1String("units") && m_resolved) {
        fetchWeather();
        fetchOthers();
    }
}

void WeatherBackend::getDisplays() {
    emit dynamicOptionsReady(QStringLiteral("displays"), QVariantList{
        QVariantMap{ { "id", "current"  }, { "label", "CURRENT CONDITIONS" } },
        QVariantMap{ { "id", "extended" }, { "label", "EXTENDED FORECAST"  } },
        QVariantMap{ { "id", "others"   }, { "label", "OTHER LOCATIONS"    } },
        QVariantMap{ { "id", "almanac"  }, { "label", "ALMANAC"            } },
        QVariantMap{ { "id", "radar"    }, { "label", "RADAR"              } },
    });
}

void WeatherBackend::emitError(const QString &reason) {
    QTimer::singleShot(0, this, [this, reason]() { emit locationError(reason); });
}

// ── Lifecycle ────────────────────────────────────────────────────────────────

void WeatherBackend::start() {
    // The saved zoom, once per process; after that zoomRadar() owns it.
    if (!m_radarZoomLoaded) {
        m_radarZoomLoaded = true;
        const int saved = moduleConfig()["radar_zoom"].toVariant().toInt();
        m_radarRequestedZoom = saved >= kRadarMinZoom && saved <= kRadarMaxZoom
                                   ? saved : kRadarDefaultZoom;
        m_radarZoom = m_radarRequestedZoom;
    }
    if (m_resolved) {
        // Already resolved from an earlier visit: refresh both requests. Only
        // fetching the primary here left Other Locations showing whatever units
        // were in force last time the module was open.
        fetchWeather();
        fetchOthers();
        fetchObservation();
        fetchOtherObservations();
        fetchRadar();
        m_refresh->start();
        return;
    }
    resolveLocation();
}

void WeatherBackend::stop() {
    m_refresh->stop();
}

// ── Music ────────────────────────────────────────────────────────────────────

QStringList WeatherBackend::musicPlaylist() const {
    QStringList entries = readListFile(music_file_path());
    if (entries.isEmpty()) {
        for (const char *track : kBuiltInMusic)
            entries << QString::fromUtf8(track);
    }

    QStringList out;
    for (QString entry : entries) {
        if (entry.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
            || entry.startsWith(QLatin1String("https://"), Qt::CaseInsensitive)) {
            // Only URLs get encoded. A local path with a space is already valid
            // as-is, and percent-encoding one turns it into a missing file.
            out << entry.replace(QLatin1Char(' '), QLatin1String("%20"));
        } else {
            // Relative paths resolve against the data dir, so a user can drop
            // music beside weather_music.txt and name it with one word.
            out << (QFileInfo(entry).isAbsolute() ? entry
                                                  : QDir(m_dataRoot).filePath(entry));
        }
    }
    return out;
}

bool WeatherBackend::musicPlaying() const {
    return m_music && m_music->state() != QProcess::NotRunning && !m_musicPaused;
}

void WeatherBackend::startMusic() {
    // Toggles are stored as JSON booleans, but tolerate the manifest's "ON"
    // spelling in case a config was written before that convention.
    const QJsonValue setting = moduleConfig()[QStringLiteral("music")];
    const bool enabled = setting.isString()
                             ? setting.toString().compare(QLatin1String("ON"),
                                                          Qt::CaseInsensitive) == 0
                             : setting.toBool(true);
    if (!enabled) return;
    if (m_music) return;

    const QStringList tracks = musicPlaylist();
    if (tracks.isEmpty()) return;

    const QString bin = mpvbin::locate();
    if (bin.isEmpty()) {
        qWarning("[Weather] mpv not found — music will not play");
        return;
    }

    QStringList args;
    args << QStringLiteral("--no-video")
         << QStringLiteral("--loop-playlist=inf")
         // Shuffled once at load and looped forever, so the app never has to
         // track a current track.
         << QStringLiteral("--shuffle")
         << QStringLiteral("--no-terminal")
         << QStringLiteral("--really-quiet")
         << QStringLiteral("--input-ipc-server=%1").arg(m_musicSocketPath)
         // Everything after this is a file, so an entry starting with '-'
         // cannot be mistaken for an option.
         << QStringLiteral("--")
         << tracks;

    m_musicPaused = false;
    m_music = new QProcess(this);
    // mpv exiting on its own (no network, no readable files) leaves a stale
    // pointer that would make startMusic() a no-op on the next visit.
    connect(m_music, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        qInfo("[Weather] music process exited");
        stopMusic();
    });
    m_music->start(bin, args);

    // mpv creates the socket a beat after launch, so a single connect attempt
    // loses the race. Same retry shape as MpvController.
    if (!m_musicIpc) {
        m_musicIpc = new QLocalSocket(this);
        connect(m_musicIpc, &QLocalSocket::connected, this, [this]() {
            m_musicConnect->stop();
        });
    }
    if (!m_musicConnect) {
        m_musicConnect = new QTimer(this);
        m_musicConnect->setInterval(100);
        connect(m_musicConnect, &QTimer::timeout, this, [this]() {
            if (m_musicIpc->state() == QLocalSocket::ConnectedState
                || m_musicIpc->state() == QLocalSocket::ConnectingState)
                return;
            m_musicIpc->connectToServer(m_musicSocketPath);
        });
    }
    m_musicConnect->start();

    qInfo("[Weather] music started — %lld track(s)", static_cast<long long>(tracks.size()));
    emit musicStateChanged();
}

void WeatherBackend::stopMusic() {
    if (m_musicConnect) m_musicConnect->stop();
    if (m_musicIpc)     m_musicIpc->abort();

    if (m_music) {
        QProcess *p = m_music;
        m_music = nullptr;          // cleared first: terminate() can re-enter
        p->disconnect(this);        // this slot via the finished signal
        if (p->state() != QProcess::NotRunning) {
            p->terminate();
            p->waitForFinished(1000);
        }
        p->deleteLater();
    }
    m_musicPaused = false;
    emit musicStateChanged();
}

void WeatherBackend::toggleMusic() {
    if (!m_music || m_music->state() == QProcess::NotRunning) return;
    if (m_musicIpc->state() != QLocalSocket::ConnectedState) {
        qWarning("[Weather] music IPC not connected — ignoring toggle");
        return;
    }
    m_musicPaused = !m_musicPaused;
    // set_property, not "cycle pause": our idea of the state and mpv's can then
    // never drift apart.
    const QJsonObject cmd{
        { QStringLiteral("command"),
          QJsonArray{ QStringLiteral("set_property"), QStringLiteral("pause"),
                      m_musicPaused } }
    };
    m_musicIpc->write(QJsonDocument(cmd).toJson(QJsonDocument::Compact) + "\n");
    emit musicStateChanged();
}

// ── Location ─────────────────────────────────────────────────────────────────

QStringList WeatherBackend::readListFile(const QString &path) const {
    QStringList lines;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return lines;
    while (!f.atEnd()) {
        const QString candidate = QString::fromUtf8(f.readLine()).trimmed();
        // '#' comments exist so the files we tell users to create can document
        // their own format.
        if (candidate.isEmpty() || candidate.startsWith(QLatin1Char('#'))) continue;
        lines << candidate;
    }
    return lines;
}

QStringList WeatherBackend::readLocationLines() const {
    return readListFile(location_file_path());
}

// "42.3601, -71.0589" or "42.3601, -71.0589, BOSTON".
//
// Explicit coordinates are the reliable escape hatch when a place name is
// ambiguous — they never touch the geocoder, so nothing can mis-resolve them.
// The optional label exists because a name cannot be recovered from
// coordinates: Open-Meteo's geocoder is forward-only, and the forecast response
// carries only a timezone, which would label a Massachusetts town "NEW YORK".
bool WeatherBackend::parseCoordLine(const QString &line, double *lat, double *lon,
                                    QString *label) {
    const QStringList parts = line.split(QLatin1Char(','));
    if (parts.size() < 2) return false;
    bool okLat = false, okLon = false;
    const double la = parts.at(0).trimmed().toDouble(&okLat);
    const double lo = parts.at(1).trimmed().toDouble(&okLon);
    if (!okLat || !okLon) return false;
    if (la < -90.0 || la > 90.0 || lo < -180.0 || lo > 180.0) return false;

    const QString given = parts.mid(2).join(QLatin1Char(',')).trimmed();
    *lat = la;
    *lon = lo;
    *label = given.isEmpty()
        ? QStringLiteral("%1, %2").arg(la, 0, 'f', 4).arg(lo, 0, 'f', 4)
        : given.toUpper();
    return true;
}

void WeatherBackend::resolveLocation() {
    QFile probe(location_file_path());
    if (!probe.exists())                                    { emitError(QStringLiteral("missing"));    return; }
    if (!probe.open(QIODevice::ReadOnly | QIODevice::Text)) { emitError(QStringLiteral("unreadable")); return; }
    probe.close();

    const QStringList lines = readLocationLines();
    if (lines.isEmpty()) { emitError(QStringLiteral("empty")); return; }

    const QString primary = lines.first();
    const QStringList others = lines.mid(1);

    double lat = 0.0, lon = 0.0;
    QString label;
    if (parseCoordLine(primary, &lat, &lon, &label)) {
        m_locationName = label;
        m_lat = lat;
        m_lon = lon;
        m_resolved = true;
        m_resolvedFrom = primary;
        qInfo("[Weather] using explicit coordinates -> %s (%.4f, %.4f)",
              qPrintable(m_locationName), m_lat, m_lon);
        fetchWeather();
        resolveStations();
        fetchRadar();
        m_refresh->start();
        resolveOthers(others);
        return;
    }

    geocodeLine(primary, [this, primary, others](bool ok, QString name,
                                                 double la, double lo) {
        if (!ok) { emitError(name); return; }   // name carries the reason here
        m_locationName = name;
        m_lat = la;
        m_lon = lo;
        m_resolved = true;
        m_resolvedFrom = primary;
        qInfo("[Weather] resolved \"%s\" -> %s (%.4f, %.4f)",
              qPrintable(primary), qPrintable(m_locationName), m_lat, m_lon);
        fetchWeather();
        resolveStations();
        fetchRadar();
        m_refresh->start();
        resolveOthers(others);
    });
}

// Geocodes one "Name, qualifier, ..." line. Reports failure through `done` with
// ok=false and the reason in the name argument, so callers decide whether a
// failure is fatal (the primary location) or just skipped (an extra).
void WeatherBackend::geocodeLine(const QString &line,
                                 const std::function<void(bool, QString, double, double)> &done) {
    // Open-Meteo's geocoder matches a bare place name, so send only the part
    // before the first comma. Everything after it disambiguates the results.
    const QStringList segments = line.split(QLatin1Char(','));
    const QString name = segments.first().trimmed();
    QStringList qualifiers;
    for (int i = 1; i < segments.size(); ++i) {
        const QString q = segments.at(i).trimmed();
        if (!q.isEmpty()) qualifiers << q;
    }

    QUrl url(QString::fromLatin1(kGeocodeUrl));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("name"),     name);
    q.addQueryItem(QStringLiteral("count"),    QStringLiteral("10"));
    q.addQueryItem(QStringLiteral("language"), QStringLiteral("en"));
    q.addQueryItem(QStringLiteral("format"),   QStringLiteral("json"));
    url.setQuery(q);

    QNetworkReply *reply = m_nam->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, line, name, qualifiers, done]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            qWarning("[Weather] geocode failed for \"%s\": %s",
                     qPrintable(line), qPrintable(reply->errorString()));
            done(false, QStringLiteral("network"), 0.0, 0.0);
            return;
        }
        const QJsonArray results =
            QJsonDocument::fromJson(reply->readAll()).object()["results"].toArray();
        if (results.isEmpty()) {
            done(false, QStringLiteral("notfound"), 0.0, 0.0);
            return;
        }

        // Score every result against every qualifier and take the best. Results
        // arrive ordered by population, so a plain first-match would always pick
        // the biggest city of that name regardless of the state or country
        // asked for — "Portland, ME, USA" would land in Oregon.
        QJsonObject chosen = results.first().toObject();
        if (!qualifiers.isEmpty()) {
            int bestScore = -1;
            for (const QJsonValue &v : results) {
                const QJsonObject o = v.toObject();
                int score = 0;
                for (const QString &qq : qualifiers)
                    if (qualifierMatches(o, qq)) ++score;
                // Strictly greater keeps population order as the tie-break.
                if (score > bestScore) { bestScore = score; chosen = o; }
            }
            if (bestScore == 0)
                qWarning("[Weather] no result matched any qualifier in \"%s\" — "
                         "falling back to the most populous match", qPrintable(line));
        }
        done(true, chosen["name"].toString(name).toUpper(),
             chosen["latitude"].toDouble(), chosen["longitude"].toDouble());
    });
}

// Extras are resolved once, then fetched together. A failure here is skipped
// rather than fatal: one unrecognised extra should not take out the whole
// module, and the primary location is what the rest of the screens need.
void WeatherBackend::resolveOthers(const QStringList &lines) {
    m_otherPoints.clear();
    m_pendingOthers = 0;
    if (lines.isEmpty()) { fetchOthers(); return; }

    // Slots are pre-allocated and filled by index, not appended on completion:
    // the geocode calls run concurrently and finish in arbitrary order, so
    // appending put the rows in whatever order the network happened to answer.
    // The listed order is the user's, and it should survive.
    m_otherPoints = QVariantList(lines.size());

    for (int i = 0; i < lines.size(); ++i) {
        const QString line = lines.at(i);
        double lat = 0.0, lon = 0.0;
        QString label;
        if (parseCoordLine(line, &lat, &lon, &label)) {
            m_otherPoints[i] = QVariantMap{
                { "name", label }, { "lat", lat }, { "lon", lon } };
            continue;
        }
        ++m_pendingOthers;
        geocodeLine(line, [this, line, i](bool ok, QString name, double la, double lo) {
            if (ok) {
                m_otherPoints[i] = QVariantMap{
                    { "name", name }, { "lat", la }, { "lon", lo } };
            } else {
                // One unrecognised extra should not take out the module — skip
                // it and keep the rest. Its slot stays empty and is dropped below.
                qWarning("[Weather] skipping other location \"%s\" (%s)",
                         qPrintable(line), qPrintable(name));
            }
            if (--m_pendingOthers <= 0) {
                m_otherPoints.removeIf([](const QVariant &v) { return v.toMap().isEmpty(); });
                fetchOthers();
                // Only once the skipped extras are gone — the station map is
                // keyed by index into the final list.
                resolveOtherStations();
            }
        });
    }
    if (m_pendingOthers == 0) {
        m_otherPoints.removeIf([](const QVariant &v) { return v.toMap().isEmpty(); });
        fetchOthers();
        resolveOtherStations();
    }
}

// ── Weather ──────────────────────────────────────────────────────────────────

void WeatherBackend::fetchWeather() {
    if (!m_resolved) return;
    const quint64 requestGeneration = ++m_weatherRequestGeneration;
    const bool us = useUsUnits();

    QUrl url(QString::fromLatin1(kForecastUrl));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("latitude"),  QString::number(m_lat, 'f', 4));
    q.addQueryItem(QStringLiteral("longitude"), QString::number(m_lon, 'f', 4));
    q.addQueryItem(QStringLiteral("current"),
                   QStringLiteral("temperature_2m,relative_humidity_2m,dew_point_2m,"
                                  "pressure_msl,wind_speed_10m,wind_direction_10m,"
                                  "visibility,weather_code,is_day"));
    q.addQueryItem(QStringLiteral("daily"),
                   QStringLiteral("temperature_2m_min,temperature_2m_max,"
                                  "weather_code,sunrise,sunset"));
    q.addQueryItem(QStringLiteral("forecast_days"), QStringLiteral("3"));
    q.addQueryItem(QStringLiteral("timezone"), QStringLiteral("auto"));
    q.addQueryItem(QStringLiteral("temperature_unit"),
                   us ? QStringLiteral("fahrenheit") : QStringLiteral("celsius"));
    q.addQueryItem(QStringLiteral("wind_speed_unit"),
                   us ? QStringLiteral("mph") : QStringLiteral("kmh"));
    url.setQuery(q);

    QNetworkReply *reply = m_nam->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, us, requestGeneration]() {
        reply->deleteLater();
        if (requestGeneration != m_weatherRequestGeneration) return;
        if (reply->error() != QNetworkReply::NoError) {
            qWarning("[Weather] fetch failed: %s", qPrintable(reply->errorString()));
            emit fetchError(reply->errorString());
            return;
        }

        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonObject cur  = root["current"].toObject();
        if (cur.isEmpty()) {
            emit fetchError(QStringLiteral("empty response"));
            return;
        }

        m_utcOffset = root["utc_offset_seconds"].toInt();

        const double windSpeed = cur["wind_speed_10m"].toDouble();
        // Visibility is always metres regardless of the unit parameters, so it
        // is the one field converted by hand.
        const double visMetres  = cur["visibility"].toDouble();

        // Only Open-Meteo knows whether the sun is up — a station reports sky
        // and weather, not daylight — so this is cached for the observation
        // overlay to reuse rather than derived per source.
        m_isDay = cur["is_day"].toInt(1) != 0;

        QVariantMap m;
        const int code = cur["weather_code"].toInt();
        m["condition"]     = conditionForCode(code);
        // Always populated, whichever source ends up winning, so anything
        // mapping from the code has a single field to read.
        m["conditionCode"] = code;
        m["iconName"]      = iconForCode(code, m_isDay);
        m["temperature"] = QString::number(qRound(cur["temperature_2m"].toDouble())) + QStringLiteral("°");
        m["humidity"]    = QString::number(qRound(cur["relative_humidity_2m"].toDouble())) + QStringLiteral("%");
        m["dewPoint"]    = QString::number(qRound(cur["dew_point_2m"].toDouble())) + QStringLiteral("°");
        m["pressure"]    = us
            ? QString::number(cur["pressure_msl"].toDouble() * 0.0295299830714, 'f', 2)
            : QString::number(cur["pressure_msl"].toDouble(), 'f', 1) + QStringLiteral(" MB");
        m["wind"] = QStringLiteral("%1 %2")
                        .arg(cardinal(cur["wind_direction_10m"].toDouble()))
                        .arg(qRound(windSpeed));
        m["visibility"] = us
            ? QString::number(qRound(visMetres / 1609.344)) + QStringLiteral(" MI.")
            : QString::number(qRound(visMetres / 1000.0))   + QStringLiteral(" KM");

        m_current = m;
        // m_current has just been rebuilt from scratch, so any station values
        // that arrived earlier are gone. Re-apply them here rather than only
        // where the observation lands — the two requests are concurrent and
        // either can be the one that finishes last.
        applyObservation();
        buildForecast(root["daily"].toObject());
        buildAlmanac(root["daily"].toObject());
        m_hasData = true;
        emit dataChanged();
    });
}

// Web Mercator tile containing (lat, lon) at the given zoom, plus where within
// that tile the point falls (0..1 on each axis) — the standard slippy-map tile
// formula (see the OSM wiki's "Slippy map tilenames"), kept local since this
// is the only place in the app that needs it.
namespace {
void latLonToTile(double lat, double lon, int zoom, int *tileX, int *tileY,
                   double *fracX, double *fracY) {
    const double n = std::pow(2.0, zoom);
    const double latRad = qDegreesToRadians(lat);
    const double xf = (lon + 180.0) / 360.0 * n;
    const double yf = (1.0 - std::log(std::tan(latRad) + 1.0 / std::cos(latRad)) / M_PI)
                       / 2.0 * n;
    *tileX = static_cast<int>(std::floor(xf));
    *tileY = static_cast<int>(std::floor(yf));
    *fracX = xf - *tileX;
    *fracY = yf - *tileY;
}
}

int WeatherBackend::zoomRadar(int step) {
    const int zoom = qBound(kRadarMinZoom, m_radarRequestedZoom + step, kRadarMaxZoom);
    if (zoom != m_radarRequestedZoom) {
        m_radarRequestedZoom = zoom;
        emit radarRequestedZoomChanged();
        // The loop on screen stays up until the new zoom's tiles are all on
        // disk; buildRadarLoop publishes zoom, tile and frames together.
        fetchRadar();
    }
    return m_radarRequestedZoom;
}

void WeatherBackend::fetchRadar() {
    if (!m_resolved) return;
    const quint64 requestGeneration = ++m_radarRequestGeneration;
    // A real fetch always wins over background prefetching (buildRadarLoop).
    ++m_radarPrefetchGeneration;
    for (const QPointer<QNetworkReply> &r : std::as_const(m_radarPrefetchReplies))
        if (r) r->abort();
    m_radarPrefetchReplies.clear();
    const int zoom = m_radarRequestedZoom;

    QNetworkReply *reply = m_nam->get(QNetworkRequest(QUrl(QString::fromLatin1(kRadarMapsUrl))));
    connect(reply, &QNetworkReply::finished, this, [this, reply, requestGeneration, zoom]() {
        reply->deleteLater();
        if (requestGeneration != m_radarRequestGeneration) return;
        if (reply->error() != QNetworkReply::NoError) {
            // Non-fatal: no formal SLA on this free endpoint (see kRadarMapsUrl's
            // comment). Radar just drops out of the display ring like an empty
            // Other Locations, rather than taking the rest of the module down.
            qWarning("[Weather] radar fetch failed: %s", qPrintable(reply->errorString()));
            return;
        }

        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        const QString host = root["host"].toString();
        const QJsonArray past = root["radar"].toObject()["past"].toArray();
        if (host.isEmpty() || past.isEmpty()) return;

        QStringList paths;
        QList<qint64> times;
        for (const QJsonValue &v : past) {
            const QString path = v.toObject()["path"].toString();
            if (path.isEmpty()) continue;
            paths << path;
            times << static_cast<qint64>(v.toObject()["time"].toDouble());
        }
        if (!paths.isEmpty())
            buildRadarLoop(host, paths, times, zoom, requestGeneration, false);
    });
}

void WeatherBackend::buildRadarLoop(const QString &host, const QStringList &paths,
                                    const QList<qint64> &frameTimes, int zoom,
                                    quint64 requestGeneration, bool prefetch) {
    // A prefetch (the zoom levels either side of the one on screen) only fills
    // the cache and never publishes. It's dropped as soon as any real fetch
    // starts (fetchRadar() aborts it), so it never competes with a zoom the
    // user is actually waiting on.
    const quint64 prefetchGeneration = m_radarPrefetchGeneration;
    const auto stale = [this, prefetch, requestGeneration, prefetchGeneration]() {
        return prefetch ? prefetchGeneration != m_radarPrefetchGeneration
                        : requestGeneration != m_radarRequestGeneration;
    };

    int tileX = 0, tileY = 0;
    double markerX = 0.5, markerY = 0.5;
    latLonToTile(m_lat, m_lon, zoom, &tileX, &tileY, &markerX, &markerY);

    // Every tile, radar and cloud, is downloaded to disk before the loop is
    // published, and both lists are published together. Handing QML remote
    // radar URLs next to local cloud files meant the radar half of each frame
    // was still loading (or re-downloading, once Qt's pixmap cache evicted the
    // 39 tiles) while the clouds were already up, so the two drifted apart.
    // On disk, a refresh also only downloads what's new since the last one.
    // Lives in the data directory, like every other file this app writes;
    // anything older than the sweep below is deleted, so it stays small.
    const QString cacheDir = m_dataRoot + QStringLiteral("/weather_radar_cache");
    QDir().mkpath(cacheDir);
    const QDateTime staleBefore = QDateTime::currentDateTimeUtc().addSecs(-6 * 3600);
    for (const QFileInfo &fi : QDir(cacheDir).entryInfoList(QDir::Files))
        if (fi.lastModified().toUTC() < staleBefore)
            QFile::remove(fi.absoluteFilePath());

    const auto iso = [](qint64 secs) {
        return QDateTime::fromSecsSinceEpoch(secs, QTimeZone::UTC).toString(Qt::ISODate);
    };
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const QUrl domainsUrl(QString::fromLatin1(kCloudDomainsUrl)
                              .arg(QString::fromLatin1(kCloudLayer),
                                   iso(frameTimes.first() - 3 * 3600), iso(now + 3600)));

    QNetworkReply *reply = m_nam->get(QNetworkRequest(domainsUrl));
    if (prefetch) m_radarPrefetchReplies << reply;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, host, paths, frameTimes, zoom, tileX, tileY, markerX, markerY,
             requestGeneration, prefetch, stale, cacheDir, iso]() {
        reply->deleteLater();
        if (stale()) return;

        // <Domain>start/end/PT10M</Domain>, possibly several comma-separated
        // ranges if GIBS has a gap; the end of the last one is the newest frame.
        // Any failure here just means a radar loop without clouds.
        qint64 newestSecs = -1;
        if (reply->error() == QNetworkReply::NoError) {
            const QRegularExpressionMatch m =
                QRegularExpression(QStringLiteral("<Domain>([^<]+)</Domain>"))
                    .match(QString::fromUtf8(reply->readAll()));
            if (m.hasMatch()) {
                const QStringList range =
                    m.captured(1).split(QLatin1Char(',')).last().split(QLatin1Char('/'));
                const QDateTime newest = QDateTime::fromString(range.value(1), Qt::ISODate);
                if (newest.isValid()) newestSecs = newest.toSecsSinceEpoch();
            }
        } else {
            qWarning("[Weather] cloud time list failed: %s", qPrintable(reply->errorString()));
        }

        // GIBS runs ~20-40 min behind RainViewer. Radar frames newer than the
        // newest satellite frame are dropped from the loop rather than paired
        // with a stale cloud frame, which read as the clouds freezing while
        // the rain kept moving. Every remaining frame gets the satellite frame
        // for its own 10-minute slot.
        int keep = frameTimes.size();
        if (newestSecs >= 0) {
            keep = 0;
            while (keep < frameTimes.size()
                   && frameTimes[keep] - frameTimes[keep] % kCloudStepSecs <= newestSecs)
                ++keep;
            if (keep == 0) {           // no overlap at all: radar alone
                keep = frameTimes.size();
                newestSecs = -1;
            }
        }
        const bool withClouds = newestSecs >= 0;

        // Cloud tiles come from zoom <= kCloudMaxZoom; deeper, from the parent
        // tile at kCloudMaxZoom, cropped to this tile's share of it.
        const int cloudZoom = qMin(zoom, kCloudMaxZoom);
        const int cloudShift = zoom - cloudZoom;
        const int radarZoom = qMin(zoom, kRadarSourceMaxZoom);
        const int radarShift = zoom - radarZoom;
        // cropDiv: tiles per parent side (1 = use the tile as-is); cropX/Y:
        // this tile's position within its parent.
        struct Job { QUrl url; QString path; bool infrared; int cropDiv; int cropX; int cropY; };
        // Per frame, so the downloads can be queued newest frame first (see
        // the early publish below).
        QList<QList<Job>> frameJobs(keep);
        const auto queued = [&frameJobs](const QString &path) {
            for (const QList<Job> &fj : frameJobs)
                for (const Job &j : fj)
                    if (j.path == path) return true;
            return false;
        };
        // Grid rows: start one row up when the location sits in the top half
        // of its tile, clamped to the map's first/last row.
        const int lastRow = (1 << zoom) - 1;
        const int topRow = qBound(0, markerY < 0.5 ? tileY - 1 : tileY, lastRow - (kMapRows - 1));
        const int centerRow = tileY - topRow;
        const int centerIndex = centerRow * kMapColumns + kMapColumns / 2;

        QList<QStringList> radarPaths, cloudPaths;   // [frame][row * kMapColumns + column]
        for (int i = 0; i < keep; ++i) {
            const QString frameId = paths[i].section(QLatin1Char('/'), -1);
            const qint64 slot = frameTimes[i] - frameTimes[i] % kCloudStepSecs;
            QStringList radarCols, cloudCols;
            for (int cell = 0; cell < kMapRows * kMapColumns; ++cell) {
                const int col = cell % kMapColumns;
                const int ty = topRow + cell / kMapColumns;
                const int tx = columnTileX(tileX, col, zoom);
                const QString rp = QStringLiteral("%1/nws_%2_%3_%4_%5.png")
                                       .arg(cacheDir, frameId).arg(zoom).arg(tx).arg(ty);
                radarCols << rp;
                if (!QFileInfo::exists(rp)) {
                    const int sub = 1 << radarShift;
                    frameJobs[i].append({QUrl(QStringLiteral("%1%2/%3/%4/%5/%6/%7/%8.png")
                                          .arg(host, paths[i], QString::number(kRadarTileSize),
                                               QString::number(radarZoom),
                                               QString::number(tx >> radarShift),
                                               QString::number(ty >> radarShift),
                                               QString::fromLatin1(kRadarColorScheme),
                                               QString::fromLatin1(kRadarTileOptions))),
                                  rp, false, sub, tx % sub, ty % sub});
                }
                if (!withClouds) continue;
                const QString cp = QStringLiteral("%1/cloud_%2_%3_%4_%5.png")
                                       .arg(cacheDir).arg(zoom).arg(tx).arg(ty).arg(slot);
                cloudCols << cp;
                if (!QFileInfo::exists(cp) && !queued(cp)) {
                    const int sub = 1 << cloudShift;
                    frameJobs[i].append({QUrl(QString::fromLatin1(kCloudTileUrl)
                                          .arg(QString::fromLatin1(kCloudLayer), iso(slot))
                                          .arg(cloudZoom).arg(ty >> cloudShift)
                                          .arg(tx >> cloudShift)),
                                  cp, true, sub, tx % sub, ty % sub});
                }
            }
            radarPaths << radarCols;
            cloudPaths << cloudCols;
        }

        QList<Job> jobs;
        for (int i = keep - 1; i >= 0; --i)
            jobs << frameJobs[i];

        // newestOnly publishes just the newest frame, as soon as it's in: a
        // zoom to a level that isn't cached yet shows the map right away and
        // the rest of the loop fills in when it lands, rather than everything
        // waiting behind the slowest of ~150 downloads.
        const auto publish = [this, radarPaths, cloudPaths, withClouds, stale,
                              zoom, tileX, tileY, markerX, markerY, centerRow,
                              centerIndex](bool newestOnly) {
            if (stale()) return;
            const auto toUrls = [](const QStringList &cols) {
                QStringList urls;
                for (const QString &p : cols)
                    urls << (QFileInfo::exists(p) ? QUrl::fromLocalFile(p).toString() : QString());
                return urls;
            };
            // GIBS lists a new time slot a few minutes before every tile for it
            // is actually up (they 404 briefly). Trailing frames whose own
            // (middle) cloud tile isn't there yet are dropped, same reasoning as
            // the trim above; they're retried on the next refresh since failures
            // aren't cached. If that would leave nothing, run without clouds.
            int last = radarPaths.size();
            bool clouded = withClouds;
            if (clouded) {
                while (last > 0 && !QFileInfo::exists(cloudPaths[last - 1].value(centerIndex)))
                    --last;
                if (last == 0) { last = radarPaths.size(); clouded = false; }
            }
            // A frame whose own (middle) radar tile never arrived is left out
            // entirely, clouds included, so the two lists keep the same index.
            // A cloud tile missing mid-loop reuses the previous frame's tile for
            // that column, a brief hold instead of the clouds blinking out.
            QVariantList radar, clouds;
            QStringList lastCloud(kMapRows * kMapColumns);
            for (int i = newestOnly ? last - 1 : 0; i < last; ++i) {
                if (!QFileInfo::exists(radarPaths[i].value(centerIndex))) continue;
                radar.append(QVariant(toUrls(radarPaths[i])));
                if (!clouded) continue;
                QStringList urls = toUrls(cloudPaths[i]);
                for (int col = 0; col < urls.size(); ++col) {
                    if (urls[col].isEmpty()) urls[col] = lastCloud[col];
                    else lastCloud[col] = urls[col];
                }
                clouds.append(QVariant(urls));
            }
            m_radarFrames = radar;
            m_cloudFrames = clouds;
            m_radarZoom = zoom;
            m_radarTileX = tileX;
            m_radarTileY = tileY;
            m_radarMarkerX = markerX;
            m_radarMarkerY = markerY;
            m_radarCenterRow = centerRow;
            emit dataChanged();
        };

        // Once the loop on screen is complete, warm the cache one zoom level
        // either side, so the next Up/Down press finds its tiles already there.
        const auto finish = [this, publish, prefetch, stale, host, paths, frameTimes,
                             zoom, requestGeneration]() {
            if (prefetch || stale()) return;
            publish(false);
            for (int z : { zoom + 1, zoom - 1 })
                if (z >= kRadarMinZoom && z <= kRadarMaxZoom)
                    buildRadarLoop(host, paths, frameTimes, z, requestGeneration, true);
        };
        if (jobs.isEmpty()) { finish(); return; }

        // Only worth it when the zoom on screen is about to change (or nothing
        // is on screen yet); a refresh at the same zoom keeps the old loop up.
        const bool earlyWanted = !prefetch && (zoom != m_radarZoom || m_radarFrames.isEmpty());
        const QStringList newestTiles = radarPaths.last() + cloudPaths.last();
        auto earlyDone = std::make_shared<bool>(!earlyWanted);

        auto pending = std::make_shared<int>(jobs.size());
        for (const Job &job : jobs) {
            QNetworkReply *tile = m_nam->get(QNetworkRequest(job.url));
            if (prefetch) m_radarPrefetchReplies << tile;
            connect(tile, &QNetworkReply::finished, this,
                    [tile, job, pending, publish, finish, newestTiles, earlyDone]() {
                tile->deleteLater();
                const QByteArray bytes = tile->readAll();
                QImage img;
                if (tile->error() == QNetworkReply::OperationCanceledError) {
                    // An aborted prefetch; nothing to report.
                } else if (tile->error() != QNetworkReply::NoError || !img.loadFromData(bytes)) {
                    qWarning("[Weather] %s tile failed: %s", job.infrared ? "cloud" : "radar",
                             qPrintable(tile->errorString()));
                } else {
                    // Radar is decoded first: the upscale below blends colors,
                    // and blended colors no longer match the palette table.
                    if (!job.infrared)
                        img = recolorRadar(img);
                    if (job.cropDiv > 1) {
                        const int side = img.width() / job.cropDiv;
                        img = img.copy(job.cropX * side, job.cropY * side, side, side)
                                 .scaled(img.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    }
                    if (job.infrared)
                        img = cloudOverlayFromInfrared(img);
                    img.save(job.path, "PNG");
                }
                if (--*pending == 0) {
                    finish();
                } else if (!*earlyDone
                           && std::all_of(newestTiles.cbegin(), newestTiles.cend(),
                                          [](const QString &p) { return QFileInfo::exists(p); })) {
                    *earlyDone = true;
                    publish(true);
                }
            });
        }
    });
}

// Exposed so Radar.qml can project its bundled MapLines.js coastline/border
// lines into the same tile-local fraction space latLonToTile() already gives
// markerX/markerY, without duplicating the zoom constant or tile size in QML.
int WeatherBackend::radarTileSourcePx() const { return kRadarTileSize; }
int WeatherBackend::radarGridColumns() const { return kMapColumns; }

// ── Station observations ─────────────────────────────────────────────────────
//
// Open-Meteo's weather_code is model output for a grid cell of several km, not
// an observation, and its sky codes (0–3) carry an implicit "and nothing is
// falling" — so a small timing error in the model reads on screen as OVERCAST
// while it is raining outside. Where the NWS has a station nearby, it reports
// what is actually happening, and reports sky and present weather separately.
//
// This runs alongside fetchWeather() rather than instead of it: the forecast
// and almanac screens need Open-Meteo regardless, so overlaying the observation
// makes the fallback free. Nothing here is fatal — outside the US /points 404s
// and the module simply keeps the model's values, which is what every non-US
// location gets.

// Two hops: the grid point names a station list, the list is ordered
// nearest-first. Answers with an empty list for anywhere the NWS doesn't cover
// or doesn't have a station close enough — never with an error, because neither
// is one.
void WeatherBackend::resolveStationsFor(double lat, double lon,
                                        const std::function<void(const QStringList &)> &done) {
    const QUrl url(QStringLiteral("%1/%2,%3")
                       .arg(QString::fromLatin1(kNwsPointsUrl))
                       .arg(lat, 0, 'f', 4)
                       .arg(lon, 0, 'f', 4));

    QNetworkReply *reply = m_nam->get(nwsRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, lat, lon, done]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            // Expected for anywhere the NWS doesn't cover — a 404 here is the
            // normal non-US path, not a problem worth warning about.
            qInfo("[Weather] no NWS coverage for %.4f,%.4f (%s) — using model data",
                  lat, lon, qPrintable(reply->errorString()));
            done({});
            return;
        }
        const QJsonObject props =
            QJsonDocument::fromJson(reply->readAll()).object()["properties"].toObject();
        const QString stationsUrl = props["observationStations"].toString();
        if (stationsUrl.isEmpty()) { done({}); return; }

        QNetworkReply *sr = m_nam->get(nwsRequest(QUrl(stationsUrl)));
        connect(sr, &QNetworkReply::finished, this, [sr, lat, lon, done]() {
            sr->deleteLater();
            if (sr->error() != QNetworkReply::NoError) {
                qWarning("[Weather] station list failed: %s", qPrintable(sr->errorString()));
                done({});
                return;
            }
            const QJsonArray features =
                QJsonDocument::fromJson(sr->readAll()).object()["features"].toArray();

            // Already ordered nearest-first for the grid point, so this only
            // drops the ones too far away to be describing our weather.
            QStringList stations;
            for (const QJsonValue &v : features) {
                if (stations.size() >= kMaxStations) break;
                const QJsonObject f = v.toObject();
                const QJsonArray coords = f["geometry"].toObject()["coordinates"].toArray();
                if (coords.size() < 2) continue;
                const double km = distanceKm(lat, lon,
                                             coords.at(1).toDouble(), coords.at(0).toDouble());
                if (km > kMaxStationKm) continue;
                const QString id =
                    f["properties"].toObject()["stationIdentifier"].toString();
                if (!id.isEmpty()) stations << id;
            }
            if (stations.isEmpty())
                qInfo("[Weather] no reporting station within %.0f km of %.4f,%.4f — "
                      "using model data", kMaxStationKm, lat, lon);
            done(stations);
        });
    });
}

// Walks the station list until one answers with something current. Stations go
// offline for days at a time, so every failure falls through to the next
// nearest rather than giving up. Answers with an empty map when none do.
void WeatherBackend::fetchObservationChain(
        const QStringList &stations, int index,
        const std::function<void(const QVariantMap &, const QDateTime &,
                                 const QString &)> &done) {
    if (index < 0 || index >= stations.size()) { done({}, {}, {}); return; }
    const QString station = stations.at(index);

    const QUrl url(QStringLiteral("https://api.weather.gov/stations/%1/observations/latest")
                       .arg(station));

    QNetworkReply *reply = m_nam->get(nwsRequest(url));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, stations, index, station, done]() {
        reply->deleteLater();

        const auto next = [this, stations, index, station, done](const char *why) {
            qInfo("[Weather] %s at %s — trying next station", why, qPrintable(station));
            fetchObservationChain(stations, index + 1, done);
        };

        if (reply->error() != QNetworkReply::NoError) { next("observation failed"); return; }

        const QJsonObject props =
            QJsonDocument::fromJson(reply->readAll()).object()["properties"].toObject();
        if (props.isEmpty()) { next("empty observation"); return; }

        const QDateTime stamp =
            QDateTime::fromString(props["timestamp"].toString(), Qt::ISODate);
        if (!stamp.isValid()
            || stamp.secsTo(QDateTime::currentDateTimeUtc()) > kObsMaxAgeSec) {
            next("stale observation");
            return;
        }

        done(parseObservation(props), stamp, station);
    });
}

void WeatherBackend::resolveStations() {
    if (m_stationsResolved || !m_resolved) return;
    m_stationsResolved = true;   // one attempt per location, however it ends

    resolveStationsFor(m_lat, m_lon, [this](const QStringList &stations) {
        m_stations = stations;
        if (m_stations.isEmpty()) return;
        qInfo("[Weather] observation stations: %s", qPrintable(m_stations.join(", ")));
        fetchObservation();
    });
}

void WeatherBackend::fetchObservation() {
    if (m_stations.isEmpty()) return;
    fetchObservationChain(m_stations, 0,
                          [this](const QVariantMap &obs, const QDateTime &stamp,
                                 const QString &station) {
        if (obs.isEmpty()) return;   // every station tried; keep the model's values
        m_obs        = obs;
        m_obsTime    = stamp;
        m_obsStation = station;
        qInfo("[Weather] observations from %s (%s) -> %s", qPrintable(station),
              qPrintable(stamp.toString(Qt::ISODate)),
              qPrintable(conditionForCode(obs["conditionCode"].toInt())));

        applyObservation();
        emit dataChanged();
    });
}

void WeatherBackend::applyObservation() {
    if (m_obs.isEmpty() || m_current.isEmpty()) return;
    // Checked here rather than at fetch time: the refresh timer keeps running
    // while a station is down, and the last good observation must stop being
    // displayed once it ages out, not merely stop being replaced.
    if (!m_obsTime.isValid()
        || m_obsTime.secsTo(QDateTime::currentDateTimeUtc()) > kObsMaxAgeSec) return;

    const bool us = useUsUnits();
    // NWS always answers in metric — degC, km/h, metres, Pa — so this converts
    // independently of the unit parameters sent to Open-Meteo.
    const auto toF   = [](double c)  { return c * 9.0 / 5.0 + 32.0; };
    const auto has   = [this](const char *k) { return m_obs.value(QLatin1String(k)).isValid(); };
    const auto num   = [this](const char *k) { return m_obs.value(QLatin1String(k)).toDouble(); };
    const auto temp  = [&](double c) {
        return QString::number(qRound(us ? toF(c) : c)) + QStringLiteral("°");
    };

    if (has("conditionCode")) {
        const int code = m_obs["conditionCode"].toInt();
        m_current["condition"]     = conditionForCode(code);
        m_current["conditionCode"] = code;
        m_current["iconName"]      = iconForCode(code, m_isDay);
    }
    if (has("temperature")) m_current["temperature"] = temp(num("temperature"));
    if (has("dewPoint"))    m_current["dewPoint"]    = temp(num("dewPoint"));
    if (has("humidity"))
        m_current["humidity"] = QString::number(qRound(num("humidity"))) + QStringLiteral("%");
    if (has("pressure")) {
        const double hPa = num("pressure") / 100.0;   // NWS reports pascals
        m_current["pressure"] = us
            ? QString::number(hPa * 0.0295299830714, 'f', 2)
            : QString::number(hPa, 'f', 1) + QStringLiteral(" MB");
    }
    // Direction is null when the wind is calm or variable, and a bearing is the
    // half of this line that can't be omitted — leave the model's value.
    if (has("windDir") && has("windSpeed")) {
        const double kmh = num("windSpeed");
        m_current["wind"] = QStringLiteral("%1 %2")
                                .arg(cardinal(num("windDir")))
                                .arg(qRound(us ? kmh / 1.609344 : kmh));
    }
    if (has("visibility")) {
        const double m = num("visibility");
        m_current["visibility"] = us
            ? QString::number(qRound(m / 1609.344)) + QStringLiteral(" MI.")
            : QString::number(qRound(m / 1000.0))   + QStringLiteral(" KM");
    }
}

void WeatherBackend::buildForecast(const QJsonObject &daily) {
    m_forecast.clear();
    const QJsonArray times = daily["time"].toArray();
    const QJsonArray mins  = daily["temperature_2m_min"].toArray();
    const QJsonArray maxs  = daily["temperature_2m_max"].toArray();
    const QJsonArray codes = daily["weather_code"].toArray();
    if (times.isEmpty()) return;

    for (int i = 0; i < times.size(); ++i) {
        const QDate date = QDate::fromString(times.at(i).toString(), Qt::ISODate);
        QVariantMap day;
        day["name"] = date.isValid()
            ? QLocale::c().dayName(date.dayOfWeek(), QLocale::LongFormat).toUpper()
            : QString();
        // Always daytime: a forecast for a whole day is a daytime
        // characterisation, so these columns never show a moon.
        const int code = codes.at(i).toInt();
        day["condition"] = conditionShortForCode(code, true);
        day["iconName"]  = iconForCode(code, true);
        day["lo"] = QString::number(qRound(mins.at(i).toDouble()));
        day["hi"] = QString::number(qRound(maxs.at(i).toDouble()));

        m_forecast.append(day);
    }
}

// ── Moon phases ──────────────────────────────────────────────────────────────
//
// Meeus, "Astronomical Algorithms", ch. 49, truncated to the principal periodic
// terms (the planetary A1..A14 corrections contribute well under an hour and are
// omitted). Accuracy of a few minutes, which matters more than it sounds: the
// naive mean-synodic approximation is off by up to ~0.6 days, enough to move a
// phase across midnight and print the wrong date — it disagreed with a reference
// frame on two of the four phases.
//
// Open-Meteo has no moon data, so this is computed rather than fetched.
namespace {

double phaseJde(double k, double phase) {
    k += phase;
    const double T = k / 1236.85;
    double jde = 2451550.09766 + 29.530588861 * k + 0.00015437 * T * T
               - 0.000000150 * T * T * T + 0.00000000073 * T * T * T * T;

    const double E  = 1 - 0.002516 * T - 0.0000074 * T * T;
    const double M  = qDegreesToRadians(2.5534 + 29.10535670 * k
                                        - 0.0000014 * T * T - 0.00000011 * T * T * T);
    const double Mp = qDegreesToRadians(201.5643 + 385.81693528 * k
                                        + 0.0107582 * T * T + 0.00001238 * T * T * T);
    const double F  = qDegreesToRadians(160.7108 + 390.67050284 * k
                                        - 0.0016118 * T * T - 0.00000227 * T * T * T);
    const double Om = qDegreesToRadians(124.7746 - 1.56375588 * k
                                        + 0.0020672 * T * T + 0.00000215 * T * T * T);

    const bool isQuarter = (phase == 0.25 || phase == 0.75);
    if (!isQuarter) {
        const bool isNew = (phase == 0.0);
        jde += (isNew ? -0.40720 : -0.40614) * qSin(Mp)
             + (isNew ?  0.17241 :  0.17302) * E * qSin(M)
             + (isNew ?  0.01608 :  0.01614) * qSin(2 * Mp)
             + (isNew ?  0.01039 :  0.01043) * qSin(2 * F)
             + (isNew ?  0.00739 :  0.00734) * E * qSin(Mp - M)
             + (isNew ? -0.00514 : -0.00515) * E * qSin(Mp + M)
             + (isNew ?  0.00208 :  0.00209) * E * E * qSin(2 * M)
             - 0.00111 * qSin(Mp - 2 * F) - 0.00057 * qSin(Mp + 2 * F)
             + 0.00056 * E * qSin(2 * Mp + M) - 0.00042 * qSin(3 * Mp)
             + 0.00042 * E * qSin(M + 2 * F) + 0.00038 * E * qSin(M - 2 * F)
             - 0.00024 * E * qSin(2 * Mp - M) - 0.00017 * qSin(Om)
             - 0.00007 * qSin(Mp + 2 * M);
    } else {
        jde += -0.62801 * qSin(Mp) + 0.17172 * E * qSin(M)
             - 0.01183 * E * qSin(Mp + M) + 0.00862 * qSin(2 * Mp)
             + 0.00804 * qSin(2 * F) + 0.00454 * E * qSin(Mp - M)
             + 0.00204 * E * E * qSin(2 * M) - 0.00180 * qSin(Mp - 2 * F)
             - 0.00070 * qSin(Mp + 2 * F) - 0.00040 * qSin(3 * Mp)
             - 0.00034 * E * qSin(2 * Mp - M) + 0.00032 * E * qSin(M + 2 * F)
             + 0.00032 * E * qSin(M - 2 * F) - 0.00028 * E * E * qSin(Mp + 2 * M)
             + 0.00027 * E * qSin(2 * Mp + M) - 0.00017 * qSin(Om);
        const double W = 0.00306 - 0.00038 * E * qCos(M) + 0.00026 * qCos(Mp)
                       - 0.00002 * qCos(Mp - M) + 0.00002 * qCos(Mp + M)
                       + 0.00002 * qCos(2 * F);
        jde += (phase == 0.25) ? W : -W;
    }
    return jde;
}

QDateTime jdToUtc(double jd) {
    // QTimeZone::utc(), not Qt::UTC: the time-spec overload is deprecated as of
    // Qt 6.9. QTimeZone::utc() has existed since Qt 5.2, so it warns on nothing
    // — unlike QTimeZone::UTC, which would need Qt 6.5+ and break a build on a
    // distro Qt 6.4 (Debian bookworm).
    return QDateTime::fromMSecsSinceEpoch(
        qint64((jd - 2440587.5) * 86400.0 * 1000.0), QTimeZone::utc());
}

} // namespace

void WeatherBackend::buildAlmanac(const QJsonObject &daily) {
    QVariantMap almanac;
    const bool h12 = useTwelveHour();
    const QString timeFmt = h12 ? QStringLiteral("h:mm AP") : QStringLiteral("HH:mm");

    // Sunrise/sunset for the first two days. timezone=auto means these are
    // already local to the forecast location.
    const QJsonArray times   = daily["time"].toArray();
    const QJsonArray sunrise = daily["sunrise"].toArray();
    const QJsonArray sunset  = daily["sunset"].toArray();
    QVariantList days;
    for (int i = 0; i < qMin(2, times.size()); ++i) {
        const QDate date = QDate::fromString(times.at(i).toString(), Qt::ISODate);
        const QDateTime rise = QDateTime::fromString(sunrise.at(i).toString(), Qt::ISODate);
        const QDateTime set  = QDateTime::fromString(sunset.at(i).toString(),  Qt::ISODate);
        days.append(QVariantMap{
            { "name", date.isValid()
                  ? QLocale::c().dayName(date.dayOfWeek(), QLocale::LongFormat).toUpper()
                  : QString() },
            { "sunrise", rise.isValid() ? QLocale::c().toString(rise, timeFmt).toUpper() : QString() },
            { "sunset",  set.isValid()  ? QLocale::c().toString(set,  timeFmt).toUpper() : QString() },
        });
    }
    almanac["days"] = days;

    // Next occurrence of each principal phase, in chronological order.
    const QDateTime nowUtc = QDateTime::currentDateTimeUtc();
    const double jdNow = nowUtc.toMSecsSinceEpoch() / 86400000.0 + 2440587.5;
    const QDate today = nowUtc.date();
    const double kBase = std::floor((today.year() + today.dayOfYear() / 365.25 - 2000.0) * 12.3685);

    QList<QPair<double, QString>> found;
    const QList<QPair<double, QString>> wanted = {
        { 0.00, QStringLiteral("NEW")   }, { 0.25, QStringLiteral("FIRST") },
        { 0.50, QStringLiteral("FULL")  }, { 0.75, QStringLiteral("LAST")  },
    };
    for (const auto &w : wanted) {
        for (int dk = -2; dk <= 3; ++dk) {
            const double jde = phaseJde(kBase + dk, w.first);
            if (jde > jdNow) { found.append({ jde, w.second }); break; }
        }
    }
    std::sort(found.begin(), found.end(),
              [](const QPair<double, QString> &a, const QPair<double, QString> &b) {
                  return a.first < b.first;
              });

    QVariantList moons;
    for (const auto &f : found) {
        // Render in the forecast location's timezone, not UTC. A phase at
        // 02:23 UTC is the previous evening in New York — getting this wrong
        // shifts the printed date by a day for roughly a third of all phases.
        const QDate local = jdToUtc(f.first).addSecs(m_utcOffset).date();
        moons.append(QVariantMap{
            { "name", f.second },
            { "date", QLocale::c().toString(local, QStringLiteral("MMM d")).toUpper() },
        });
    }
    almanac["moons"] = moons;

    m_almanac = almanac;
}

bool WeatherBackend::useTwelveHour() const {
    return moduleConfig()["hours_format"].toString(QStringLiteral("24-hour"))
               .startsWith(QLatin1String("12"));
}

QString WeatherBackend::tempUnitLabel() const {
    return useUsUnits() ? QStringLiteral("°F") : QStringLiteral("°C");
}

// One request for every extra place. Open-Meteo accepts comma-joined
// coordinates and answers with an array in the same order — so the whole table
// is a single round trip no matter how many places are listed.
void WeatherBackend::fetchOthers() {
    const quint64 requestGeneration = ++m_otherRequestGeneration;
    if (m_otherPoints.isEmpty()) {
        m_otherLocations.clear();
        emit dataChanged();
        return;
    }
    const bool us = useUsUnits();

    QStringList lats, lons;
    for (const QVariant &v : m_otherPoints) {
        const QVariantMap m = v.toMap();
        lats << QString::number(m["lat"].toDouble(), 'f', 4);
        lons << QString::number(m["lon"].toDouble(), 'f', 4);
    }

    QUrl url(QString::fromLatin1(kForecastUrl));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("latitude"),  lats.join(QLatin1Char(',')));
    q.addQueryItem(QStringLiteral("longitude"), lons.join(QLatin1Char(',')));
    q.addQueryItem(QStringLiteral("current"),
                   QStringLiteral("temperature_2m,weather_code,wind_speed_10m,"
                                  "wind_direction_10m,is_day"));
    q.addQueryItem(QStringLiteral("temperature_unit"),
                   us ? QStringLiteral("fahrenheit") : QStringLiteral("celsius"));
    q.addQueryItem(QStringLiteral("wind_speed_unit"),
                   us ? QStringLiteral("mph") : QStringLiteral("kmh"));
    url.setQuery(q);

    QNetworkReply *reply = m_nam->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, requestGeneration]() {
        reply->deleteLater();
        if (requestGeneration != m_otherRequestGeneration) return;
        if (reply->error() != QNetworkReply::NoError) {
            qWarning("[Weather] other-locations fetch failed: %s",
                     qPrintable(reply->errorString()));
            return;
        }

        // The API returns a bare object for one coordinate and an array for
        // several, so normalise before iterating.
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        QJsonArray entries;
        if (doc.isArray())       entries = doc.array();
        else if (doc.isObject()) entries.append(doc.object());

        QVariantList rows;
        // Rebuilt together: the table skips extras the API returned nothing
        // for, so the overlay can't assume row index == point index.
        m_otherRowPoint.clear();
        m_otherIsDay.clear();
        for (int i = 0; i < entries.size() && i < m_otherPoints.size(); ++i) {
            const QJsonObject cur = entries.at(i).toObject()["current"].toObject();
            if (cur.isEmpty()) continue;
            const double speed = cur["wind_speed_10m"].toDouble();
            // Each city has its own daylight — Tokyo can be mid-morning while
            // the primary location is in the small hours. Kept for the station
            // overlay, which has no daylight of its own to report.
            const bool isDay = cur["is_day"].toInt(1) != 0;
            m_otherRowPoint.append(i);
            m_otherIsDay.insert(i, isDay);
            rows.append(QVariantMap{
                { "name", m_otherPoints.at(i).toMap()["name"] },
                { "temp", QString::number(qRound(cur["temperature_2m"].toDouble())) },
                { "condition", conditionShortForCode(cur["weather_code"].toInt(), isDay) },
                // The original showed CALM rather than a direction with no
                // speed behind it.
                { "wind", qRound(speed) == 0
                      ? QStringLiteral("CALM")
                      : QStringLiteral("%1 %2")
                            .arg(cardinal(cur["wind_direction_10m"].toDouble()))
                            .arg(qRound(speed)) },
            });
        }
        m_otherLocations = rows;
        // Same reason as the primary screen: this handler rebuilds the rows
        // from scratch and races the observation requests, so re-apply here.
        applyOtherObservations();
        emit dataChanged();
    });
}

// The extras get the same treatment as the primary location, one point at a
// time. They can't share the primary's batched Open-Meteo request — station
// observations are per-station — so a table of US cities costs one request per
// row per refresh instead of one for the whole table. That's the price of rows
// that report what is actually happening; international rows cost nothing extra
// because /points 404s once and is never asked again.
void WeatherBackend::resolveOtherStations() {
    m_otherStations.clear();
    m_otherObs.clear();
    m_otherObsTime.clear();

    for (int i = 0; i < m_otherPoints.size(); ++i) {
        const QVariantMap p = m_otherPoints.at(i).toMap();
        const QString name  = p["name"].toString();
        resolveStationsFor(p["lat"].toDouble(), p["lon"].toDouble(),
                           [this, i, name](const QStringList &stations) {
            if (stations.isEmpty()) return;   // international, or nothing close
            m_otherStations.insert(i, stations);
            qInfo("[Weather] %s observation stations: %s",
                  qPrintable(name), qPrintable(stations.join(", ")));
            fetchObservationChain(stations, 0,
                                  [this, i, name](const QVariantMap &obs,
                                                  const QDateTime &stamp,
                                                  const QString &station) {
                if (obs.isEmpty()) return;
                m_otherObs.insert(i, obs);
                m_otherObsTime.insert(i, stamp);
                qInfo("[Weather] %s observations from %s -> %s", qPrintable(name),
                      qPrintable(station),
                      qPrintable(conditionShortForCode(obs["conditionCode"].toInt(),
                                                       m_otherIsDay.value(i, true))));
                applyOtherObservations();
                emit dataChanged();
            });
        });
    }
}

void WeatherBackend::fetchOtherObservations() {
    for (auto it = m_otherStations.cbegin(); it != m_otherStations.cend(); ++it) {
        const int i = it.key();
        fetchObservationChain(it.value(), 0,
                              [this, i](const QVariantMap &obs, const QDateTime &stamp,
                                        const QString &) {
            if (obs.isEmpty()) return;
            m_otherObs.insert(i, obs);
            m_otherObsTime.insert(i, stamp);
            applyOtherObservations();
            emit dataChanged();
        });
    }
}

void WeatherBackend::applyOtherObservations() {
    if (m_otherObs.isEmpty() || m_otherLocations.isEmpty()) return;
    const bool us = useUsUnits();

    for (int row = 0; row < m_otherLocations.size() && row < m_otherRowPoint.size(); ++row) {
        const int i = m_otherRowPoint.at(row);
        if (!m_otherObs.contains(i)) continue;
        const QDateTime stamp = m_otherObsTime.value(i);
        if (!stamp.isValid()
            || stamp.secsTo(QDateTime::currentDateTimeUtc()) > kObsMaxAgeSec) continue;

        const QVariantMap obs = m_otherObs.value(i);
        QVariantMap r = m_otherLocations.at(row).toMap();

        // The abbreviated vocabulary, same as the model rows use — these
        // columns are a third of the screen wide. Daylight comes from the model
        // row underneath: the observation reports sky and weather, never
        // whether the sun is up.
        r["condition"] = conditionShortForCode(obs["conditionCode"].toInt(),
                                               m_otherIsDay.value(i, true));
        if (obs["temperature"].isValid()) {
            const double c = obs["temperature"].toDouble();
            r["temp"] = QString::number(qRound(us ? c * 9.0 / 5.0 + 32.0 : c));
        }
        if (obs["windDir"].isValid() && obs["windSpeed"].isValid()) {
            const double kmh   = obs["windSpeed"].toDouble();
            const int    speed = qRound(us ? kmh / 1.609344 : kmh);
            // The original showed CALM rather than a direction with no speed
            // behind it.
            r["wind"] = speed == 0
                ? QStringLiteral("CALM")
                : QStringLiteral("%1 %2").arg(cardinal(obs["windDir"].toDouble())).arg(speed);
        }
        m_otherLocations[row] = r;
    }
}
