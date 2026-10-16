#pragma once
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVariantList>
#include <QJsonObject>
#include <QStringList>
#include <functional>

class QNetworkAccessManager;
class QTimer;
class QProcess;
class QLocalSocket;

// Backend for the weather module.
//
// Fetches Open-Meteo directly and exposes display-ready strings. Formatting
// lives here rather than in QML because every field's presentation depends on
// the Units setting, and the screens are literally fixed lines of text — so a
// property like current.temperature is already "66°" and the view stays dumb.
//
// The location comes from $DATA_ROOT/weather_location.txt: a plain file rather
// than a setting, because the manifest schema has no free-text type and an
// on-screen keyboard for a value you set once would be worse. A place name is
// geocoded via Open-Meteo (no API key); an explicit "lat, lon" is taken as-is.
class WeatherBackend : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString     locationName     READ locationName     NOTIFY dataChanged)
    Q_PROPERTY(QVariantMap current          READ current          NOTIFY dataChanged)
    // Three days, each a map of display-ready strings: name, condition, lo, hi.
    Q_PROPERTY(QVariantList forecast        READ forecast         NOTIFY dataChanged)
    // { days: [{name, sunrise, sunset}], moons: [{name, date}] }
    Q_PROPERTY(QVariantMap  almanac         READ almanac          NOTIFY dataChanged)
    // [{name, temp, condition, wind}] for the extra places listed in
    // weather_location.txt. Empty when none are listed, which drops the screen
    // out of the rotation rather than showing an empty table.
    Q_PROPERTY(QVariantList otherLocations  READ otherLocations   NOTIFY dataChanged)
    Q_PROPERTY(bool hasOtherLocations       READ hasOtherLocations NOTIFY dataChanged)
    // "°F" or "°C", for the table's column heading.
    Q_PROPERTY(QString tempUnitLabel        READ tempUnitLabel    NOTIFY dataChanged)
    Q_PROPERTY(bool        hasData          READ hasData          NOTIFY dataChanged)
    // Location's own UTC offset, so the status-bar clock shows the time where
    // the weather is rather than where the device is.
    Q_PROPERTY(int         utcOffsetSeconds READ utcOffsetSeconds NOTIFY dataChanged)
    // Whether background music is currently audible — running and not paused.
    // Nothing binds it yet; it exists so an on-screen indicator is a QML-only
    // change.
    Q_PROPERTY(bool musicPlaying READ musicPlaying NOTIFY musicStateChanged)
    // Animated radar loop (RainViewer's public API, no key). One entry per
    // available frame, each a row-major grid of tile URLs (radarGridColumns
    // wide, north row first). The location's own tile is the middle column of
    // row radarCenterRow. See buildRadarLoop().
    Q_PROPERTY(QVariantList radarFrames READ radarFrames NOTIFY dataChanged)
    Q_PROPERTY(bool hasRadar READ hasRadar NOTIFY dataChanged)
    // Resolved location's position within that tile, 0..1 on each axis, so the
    // screen can drop a "you are here" marker at the exact spot.
    Q_PROPERTY(double radarMarkerX READ radarMarkerX NOTIFY dataChanged)
    Q_PROPERTY(double radarMarkerY READ radarMarkerY NOTIFY dataChanged)
    // The tile's own XYZ coordinates and pixel size, so the screen can project
    // its bundled coastline/border lines (lon/lat) into the same tile-local
    // space markerX/markerY already use, without duplicating this math in QML.
    Q_PROPERTY(int radarTileX READ radarTileX NOTIFY dataChanged)
    Q_PROPERTY(int radarTileY READ radarTileY NOTIFY dataChanged)
    // Zoom of the loop currently published (it changes together with the
    // frames and tile x/y, so lines and tiles never disagree mid-switch), and
    // the zoom asked for with zoomRadar(); they differ while a new zoom's
    // tiles are still downloading.
    Q_PROPERTY(int radarZoom READ radarZoom NOTIFY dataChanged)
    Q_PROPERTY(int radarRequestedZoom READ radarRequestedZoom NOTIFY radarRequestedZoomChanged)
    Q_PROPERTY(int radarTileSourcePx READ radarTileSourcePx CONSTANT)
    Q_PROPERTY(int radarGridColumns READ radarGridColumns CONSTANT)
    Q_PROPERTY(int radarCenterRow READ radarCenterRow NOTIFY dataChanged)
    // Cloud cover under the rain, laid out exactly like radarFrames (same frame
    // index matched by time, same columns), but local file URLs; "" where a
    // tile failed. See buildRadarLoop().
    Q_PROPERTY(QVariantList cloudFrames READ cloudFrames NOTIFY dataChanged)

public:
    explicit WeatherBackend(const QString &appRoot, const QString &dataRoot,
                            QObject *parent = nullptr);
    ~WeatherBackend() override;

    QString     locationName()     const { return m_locationName; }
    QVariantMap  current()  const { return m_current; }
    QVariantList forecast() const { return m_forecast; }
    QVariantMap  almanac()  const { return m_almanac; }
    QVariantList otherLocations() const { return m_otherLocations; }
    bool hasOtherLocations() const { return !m_otherLocations.isEmpty(); }
    QString tempUnitLabel() const;
    bool        hasData()          const { return m_hasData; }
    int         utcOffsetSeconds() const { return m_utcOffset; }
    QVariantList radarFrames() const { return m_radarFrames; }
    bool   hasRadar()    const { return !m_radarFrames.isEmpty(); }
    double radarMarkerX() const { return m_radarMarkerX; }
    double radarMarkerY() const { return m_radarMarkerY; }
    int    radarTileX() const { return m_radarTileX; }
    int    radarTileY() const { return m_radarTileY; }
    int    radarZoom() const { return m_radarZoom; }
    int    radarRequestedZoom() const { return m_radarRequestedZoom; }
    int    radarTileSourcePx() const;
    int    radarGridColumns() const;
    int    radarCenterRow() const { return m_radarCenterRow; }
    QVariantList cloudFrames() const { return m_cloudFrames; }

    bool musicPlaying() const;

    // Absolute path to weather_location.txt, so the setup screen can say exactly
    // where to put it.
    Q_INVOKABLE QString location_file_path() const;
    // Absolute path to the optional weather_music.txt playlist. The app never
    // writes this file — it is opt-in, and absent means the built-in list.
    Q_INVOKABLE QString music_file_path() const;

    // Resolve the location (if not already) and fetch, then keep refreshing
    // until stop(). Answers with dataChanged() or locationError().
    Q_INVOKABLE void start();
    // Steps the radar zoom (+1 in, -1 out) within the supported range and
    // rebuilds the loop at the new zoom. Returns the zoom now requested, for
    // the view to save as the radar_zoom setting.
    Q_INVOKABLE int zoomRadar(int step);
    Q_INVOKABLE void stop();

    // Background music, as a plain mpv subprocess — no video, no screen takeover.
    // A no-op when the Music setting is off, so the view can call it
    // unconditionally.
    Q_INVOKABLE void startMusic();
    Q_INVOKABLE void stopMusic();
    // Pause/resume in place over mpv's IPC socket, so the current track survives
    // the toggle. Session-only: it deliberately does not write the setting.
    Q_INVOKABLE void toggleMusic();

    // options_slot for the "displays" multiselect. Ids match Weather.qml's
    // allScreens list.
    Q_INVOKABLE void getDisplays();

signals:
    void radarRequestedZoomChanged();
    void dataChanged();
    void musicStateChanged();
    // reason ∈ {missing, unreadable, empty, notfound, network}
    void locationError(const QString &reason);
    void fetchError(const QString &message);
    void dynamicOptionsReady(const QString &key, const QVariant &options);

public slots:
    void onSettingChanged(const QString &moduleId, const QString &key, const QVariant &value);

private:
    QJsonObject loadConfig() const;
    QJsonObject moduleConfig() const;
    bool        useUsUnits() const;

    void resolveLocation();
    void geocode(const QString &rawLine);
    // '#'-commented, one-entry-per-line config files: the location list and the
    // music playlist share the format, so they share the reader.
    QStringList readListFile(const QString &path) const;
    // Shared by the primary location and the extras.
    QStringList readLocationLines() const;
    // The music playlist: weather_music.txt if present, otherwise the built-in
    // list. Entries are mpv-ready — URLs percent-encoded, local paths absolute.
    QStringList musicPlaylist() const;
    static bool parseCoordLine(const QString &line, double *lat, double *lon, QString *label);
    void geocodeLine(const QString &line,
                     const std::function<void(bool, QString, double, double)> &done);
    void resolveOthers(const QStringList &lines);
    void fetchOthers();
    void fetchWeather();
    // RainViewer's public radar-frame list, no API key. Non-fatal on failure —
    // radar just drops out of the display ring like an empty Other Locations.
    void fetchRadar();
    // Downloads every radar tile plus the GOES-East infrared satellite tiles
    // (NASA GIBS, no key) for the same tiles and timestamps, the latter turned
    // into a transparent cloud overlay, then publishes both loops at once.
    // Cloud failures are non-fatal: the loop just runs without them.
    void buildRadarLoop(const QString &host, const QStringList &paths,
                        const QList<qint64> &frameTimes, int zoom,
                        quint64 requestGeneration);

    // Station observations (US only). Resolved once per location, then polled
    // alongside fetchWeather(). Every failure here is non-fatal: the module
    // keeps whatever Open-Meteo already put in m_current.
    //
    // Both the primary location and the Other Locations extras use the same two
    // steps, so they are parameterised rather than duplicated: resolve nearby
    // stations for a point, then walk them until one answers with something
    // current.
    void resolveStationsFor(double lat, double lon,
                            const std::function<void(const QStringList &)> &done);
    void fetchObservationChain(const QStringList &stations, int index,
                               const std::function<void(const QVariantMap &,
                                                        const QDateTime &,
                                                        const QString &)> &done);

    void resolveStations();
    void fetchObservation();
    void applyObservation();

    void resolveOtherStations();
    void fetchOtherObservations();
    void applyOtherObservations();
    void buildForecast(const QJsonObject &daily);
    void buildAlmanac(const QJsonObject &daily);
    bool useTwelveHour() const;

    // Always answer on the next event-loop turn, never synchronously — views
    // call start() from Component.onCompleted, and a synchronous reply there
    // fires while the Loader's item is still being constructed, so the
    // setSource() that swaps in the setup screen is silently dropped.
    void emitError(const QString &reason);

    QString m_appRoot;
    QString m_dataRoot;
    QNetworkAccessManager *m_nam = nullptr;
    QTimer  *m_refresh = nullptr;

    // Primary and Other Locations are independent request families. Each new
    // request invalidates older callbacks in only its own family, so replies
    // that finish out of order cannot overwrite the newest snapshot.
    quint64 m_weatherRequestGeneration = 0;
    quint64 m_otherRequestGeneration = 0;
    quint64 m_radarRequestGeneration = 0;

    // Resolved location, cached for the life of the process. Deliberately not
    // persisted: the project writes config.json only on direct user
    // interaction, and re-geocoding once per module entry is one small request.
    QString m_locationName;
    double  m_lat = 0.0;
    double  m_lon = 0.0;
    bool    m_resolved = false;
    QString m_resolvedFrom;
    // Whether the sun is up at the primary location, from Open-Meteo. Cached
    // because the station overlay needs it and an observation cannot supply it.
    bool    m_isDay = true;

    // Nearest reporting stations, nearest first, cached for the life of the
    // process for the same reason the location is: config.json is written only
    // on direct user interaction. Empty outside the US, and after any failure.
    QStringList m_stations;
    bool        m_stationsResolved = false;
    // Last good observation, kept raw (metric, as NWS always sends it) rather
    // than formatted, so a units change re-formats without a re-fetch. Applied
    // over m_current *after* every fetchWeather(), which rebuilds it wholesale
    // and would otherwise wipe the overlay depending on which reply lands last.
    QVariantMap m_obs;
    QDateTime   m_obsTime;
    QString     m_obsStation;

    // Same three things per extra location, keyed by index into m_otherPoints.
    // Sparse on purpose: an international extra simply has no entry, and the
    // model's value for that row stands.
    QHash<int, QStringList> m_otherStations;
    QHash<int, QVariantMap> m_otherObs;
    QHash<int, QDateTime>   m_otherObsTime;
    // Row → m_otherPoints index. The table skips extras the API returned
    // nothing for, so row order and point order are not the same list.
    QList<int>              m_otherRowPoint;
    // Daylight per extra, from the model row. Stations report sky and weather,
    // never whether the sun is up, so the overlay borrows it from underneath.
    QHash<int, bool>        m_otherIsDay;

    // Background music. Its own mpv, its own socket — entirely separate from
    // MpvController's, which owns the screen and must not be disturbed by it.
    QProcess     *m_music       = nullptr;
    QLocalSocket *m_musicIpc    = nullptr;
    QTimer       *m_musicConnect = nullptr;
    QString       m_musicSocketPath;
    bool          m_musicPaused = false;

    // One tile URL per available frame (oldest first), plus where the resolved
    // location falls within that tile (0..1 on each axis). Empty until the
    // first successful fetchRadar().
    QVariantList m_radarFrames;
    double      m_radarMarkerX = 0.5;
    double      m_radarMarkerY = 0.5;
    int         m_radarTileX = 0;
    int         m_radarTileY = 0;
    int         m_radarZoom = 6;
    int         m_radarCenterRow = 0;
    int         m_radarRequestedZoom = 6;
    bool        m_radarZoomLoaded = false;
    QVariantList m_cloudFrames;

    QVariantMap  m_current;
    QVariantList m_forecast;
    QVariantMap  m_almanac;
    QVariantList m_otherLocations;
    // Extra places from weather_location.txt, resolved once: {name, lat, lon}.
    QVariantList m_otherPoints;
    int          m_pendingOthers = 0;
    bool        m_hasData   = false;
    int         m_utcOffset = 0;
};
