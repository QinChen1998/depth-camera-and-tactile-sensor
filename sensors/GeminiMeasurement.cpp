#include "DepthMeasurement.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QSettings>
#include <QStorageInfo>
#include <QTemporaryFile>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <type_traits>

namespace DepthMeasurement {
namespace {
void fail(const QString &text)
{
    throw std::runtime_error(text.toUtf8().constData());
}

QString sdkLibrary()
{
    QStringList roots;
    const auto override = qEnvironmentVariable("ORBBEC_SDK_ROOT");
    if (!override.isEmpty())
        roots << override;
#ifdef Q_OS_WIN
    const QString uninstall = "HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    for (const auto format : {QSettings::Registry64Format, QSettings::Registry32Format}) {
        QSettings packages(uninstall, format);
        for (const auto &key : packages.childGroups()) {
            packages.beginGroup(key);
            const auto name = packages.value("DisplayName").toString();
            if (name.startsWith("OrbbecSDK", Qt::CaseInsensitive)
                && packages.value("DisplayVersion").toString().startsWith("2.")) {
                roots << packages.value("InstallLocation").toString()
                      << packages.value("DisplayIcon").toString().remove('"');
            }
            packages.endGroup();
        }
    }
#endif
    for (const auto &root : roots) {
        if (!QDir::isAbsolutePath(root))
            continue;
        const QFileInfo library(QDir(root).filePath("bin/OrbbecSDK.dll"));
        if (library.isFile())
            return library.absoluteFilePath();
    }
    fail(QStringLiteral("Orbbec SDK v2 was not found. Install its Windows package or set ORBBEC_SDK_ROOT."));
    return {};
}

// These value types and constants are the public Orbbec SDK v2 C ABI.
// Loading the optional SDK at runtime keeps normal builds independent of its headers and DLL.
struct SdkError;
struct Intrinsic { float fx, fy, cx, cy; int16_t width, height; };
struct Distortion { float k1, k2, k3, k4, k5, k6, p1, p2; int model; };
constexpr int DepthSensor = 3;
constexpr int Y16 = 8;
constexpr int Width = 1280;
constexpr int Height = 800;
constexpr int Fps = 15;

class Sdk {
public:
    explicit Sdk(const QString &path) : m_library(path)
    {
        if (!m_library.load())
            fail(m_library.errorString());
        const auto getVersion = symbol<int (*)()>("ob_get_version");
        version = getVersion();
        if (version / 10000 != 2)
            fail(QStringLiteral("Gemini recording requires Orbbec SDK v2"));
        m_message = symbol<const char *(*)(const SdkError *)>("ob_error_get_message");
        m_freeError = symbol<void (*)(SdkError *)>("ob_delete_error");
    }

    template<class R, class... Args>
    R call(const char *name, Args... args)
    {
        auto fn = symbol<R (*)(Args..., SdkError **)>(name);
        SdkError *error = nullptr;
        if constexpr (std::is_void_v<R>) {
            fn(args..., &error);
            check(name, error);
        } else {
            const auto result = fn(args..., &error);
            check(name, error);
            return result;
        }
    }

    using Handle = std::unique_ptr<void, std::function<void(void *)>>;
    Handle own(void *value, const char *destroy)
    {
        if (!value)
            fail(QStringLiteral("Orbbec SDK returned an empty object"));
        // Resolve the destructor before publishing a handle.
        const auto release = symbol<void (*)(void *, SdkError **)>(destroy);
        return Handle(value, [this, release](void *pointer) {
            SdkError *error = nullptr;
            release(pointer, &error);
            if (error)
                m_freeError(error);
        });
    }

    int version = 0;

private:
    template<class F>
    F symbol(const char *name)
    {
        const auto proc = m_library.resolve(name);
        if (!proc)
            fail(QStringLiteral("Missing Orbbec SDK function %1").arg(QString::fromLatin1(name)));
        return reinterpret_cast<F>(proc);
    }

    void check(const char *name, SdkError *error)
    {
        if (!error)
            return;
        const QString detail = QString::fromUtf8(m_message(error));
        m_freeError(error);
        fail(QStringLiteral("%1: %2").arg(QString::fromLatin1(name), detail));
    }

    QLibrary m_library;
    const char *(*m_message)(const SdkError *) = nullptr;
    void (*m_freeError)(SdkError *) = nullptr;
};

class GeminiInput {
public:
    explicit GeminiInput(const QString &library)
        : m_sdk(library), m_configFile(QDir::temp().filePath("dcts-orbbec-XXXXXX.xml"))
    {
        if (!m_configFile.open())
            fail(m_configFile.errorString());
        // USB capture only, without changing the vendor installation or camera presets.
        const QByteArray config("<Config><Log><FileLogLevel>5</FileLogLevel><ConsoleLogLevel>5</ConsoleLogLevel>"
                                "</Log><Device><EnumerateNetDevice>false</EnumerateNetDevice></Device></Config>");
        if (m_configFile.write(config) != config.size() || !m_configFile.flush())
            fail(m_configFile.errorString());
        m_configFile.close();
        const auto path = m_configFile.fileName().toUtf8();
        const auto extensions = QFileInfo(library).dir().filePath("extensions").toUtf8();
        m_sdk.call<void>("ob_set_extensions_directory", extensions.constData());
        m_context = m_sdk.own(m_sdk.call<void *>("ob_create_context_with_config", path.constData()), "ob_delete_context");
        m_sdk.call<void>("ob_enable_net_device_enumeration", m_context.get(), false);
        auto devices = m_sdk.own(m_sdk.call<void *>("ob_query_device_list", m_context.get()), "ob_delete_device_list");
        const auto count = m_sdk.call<uint32_t>("ob_device_list_get_count", devices.get());
        for (uint32_t i = 0; i < count; ++i) {
            const auto name = QString::fromUtf8(m_sdk.call<const char *>("ob_device_list_get_device_name", devices.get(), i));
            auto compact = name;
            compact.remove(' ');
            if (!compact.contains("Gemini335", Qt::CaseInsensitive))
                continue;
            m_device = m_sdk.own(m_sdk.call<void *>("ob_device_list_get_device", devices.get(), i), "ob_delete_device");
            m_format["device"] = name;
            break;
        }
        if (!m_device)
            fail(QStringLiteral("Gemini 335 was not found by the SDK"));
        auto info = m_sdk.own(m_sdk.call<void *>("ob_device_get_device_info", m_device.get()), "ob_delete_device_info");
        m_format["firmware"] = QString::fromUtf8(m_sdk.call<const char *>("ob_device_info_get_firmware_version", info.get()));
        m_pipeline = m_sdk.own(m_sdk.call<void *>("ob_create_pipeline_with_device", m_device.get()), "ob_delete_pipeline");
        auto profiles = m_sdk.own(m_sdk.call<void *>("ob_pipeline_get_stream_profile_list", m_pipeline.get(), DepthSensor),
                                  "ob_delete_stream_profile_list");
        auto profile = m_sdk.own(m_sdk.call<void *>("ob_stream_profile_list_get_video_stream_profile",
                                                  profiles.get(), Width, Height, Y16, Fps),
                                 "ob_delete_stream_profile");
        const auto intr = m_sdk.call<Intrinsic>("ob_video_stream_profile_get_intrinsic", profile.get());
        const auto dist = m_sdk.call<Distortion>("ob_video_stream_profile_get_distortion", profile.get());
        if (intr.width != Width || intr.height != Height || !(intr.fx > 0) || !(intr.fy > 0))
            fail(QStringLiteral("Gemini returned incompatible depth intrinsics"));
        m_format["intrinsics"] = QJsonObject{{"fx", intr.fx}, {"fy", intr.fy}, {"cx", intr.cx}, {"cy", intr.cy}};
        m_format["distortion"] = QJsonObject{{"model", dist.model}, {"k1", dist.k1}, {"k2", dist.k2},
            {"k3", dist.k3}, {"k4", dist.k4}, {"k5", dist.k5}, {"k6", dist.k6}, {"p1", dist.p1}, {"p2", dist.p2}};
        m_format["encoding"] = "y16_le";
        m_format["coordinate"] = "gemini_depth_camera_z";
        m_format["source_clock"] = "gemini_device_timestamp";
        m_format["sdk_version"] = m_sdk.version;
        m_format["width"] = Width;
        m_format["height"] = Height;
        m_format["profile_fps"] = Fps;
        auto configHandle = m_sdk.own(m_sdk.call<void *>("ob_create_config"), "ob_delete_config");
        m_sdk.call<void>("ob_config_enable_stream_with_stream_profile", configHandle.get(), profile.get());
        m_sdk.call<void>("ob_config_set_align_mode", configHandle.get(), 0);
        m_sdk.call<void>("ob_pipeline_start_with_config", m_pipeline.get(), configHandle.get());
    }

    ~GeminiInput()
    {
        if (m_pipeline) {
            try { m_sdk.call<void>("ob_pipeline_stop", m_pipeline.get()); }
            catch (const std::exception &) { /* Pipeline deletion also releases the stream. */ }
        }
    }

    std::optional<Frame> read()
    {
        auto *frames = m_sdk.call<void *>("ob_pipeline_wait_for_frameset", m_pipeline.get(), uint32_t(200));
        if (!frames)
            return std::nullopt;
        const qint64 arrival = nowNs();
        auto frameSet = m_sdk.own(frames, "ob_delete_frame");
        auto *data = m_sdk.call<void *>("ob_frameset_get_depth_frame", frameSet.get());
        if (!data)
            return std::nullopt;
        auto depth = m_sdk.own(data, "ob_delete_frame");
        if (++m_warmup < 20)
            return std::nullopt;
        const auto scale = m_sdk.call<float>("ob_depth_frame_get_value_scale", depth.get());
        const auto size = m_sdk.call<uint32_t>("ob_frame_get_data_size", depth.get());
        const auto pixels = m_sdk.call<uint8_t *>("ob_frame_get_data", depth.get());
        if (!pixels || m_sdk.call<int>("ob_frame_get_format", depth.get()) != Y16
            || size != Width * Height * 2 || !std::isfinite(scale) || scale <= 0)
            fail(QStringLiteral("Gemini returned an invalid Y16 frame"));
        auto format = m_format;
        format["scale_mm_per_code"] = scale;
        format["invalid_code"] = 0;
        const auto sequence = m_sdk.call<uint64_t>("ob_frame_get_index", depth.get());
        const auto timestamp = m_sdk.call<uint64_t>("ob_frame_get_timestamp_us", depth.get());
        const auto sdkHost = m_sdk.call<uint64_t>("ob_frame_get_system_timestamp_us", depth.get());
        return Frame{QByteArray(reinterpret_cast<const char *>(pixels), size), format,
                     {{"sdk_host_time_us", qint64(sdkHost)}},
                     qint64(sequence), qint64(timestamp * 1000), arrival};
    }

private:
    Sdk m_sdk;
    QTemporaryFile m_configFile;
    Sdk::Handle m_context, m_device, m_pipeline;
    QJsonObject m_format;
    int m_warmup = 0;
};
}

Result recordGemini(const Options &options, Job &job)
{
    std::unique_ptr<GeminiInput> input;
    return record(options, job, [&]() -> std::optional<Frame> {
        if (qEnvironmentVariable("RV_TEST_OFFLINE_ONLY") == "1")
            fail(QStringLiteral("Gemini capture is disabled during offline tests"));
        if (!input) {
            const QStorageInfo disk(options.parentDirectory);
            if (disk.isReady() && disk.bytesAvailable() >= 0
                && disk.bytesAvailable() < qint64(Width) * Height * 2 * Fps * (options.seconds + 2))
                fail(QStringLiteral("Not enough free space to record Gemini depth"));
            input = std::make_unique<GeminiInput>(sdkLibrary());
        }
        return input->read();
    });
}
}
