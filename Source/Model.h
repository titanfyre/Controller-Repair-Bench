#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <sstream>
#include <iomanip>
#include <string>
namespace Bench
{
struct NeutralWindow
{
    std::uint64_t epoch = 0, frequency = 0;
    void Reset(std::uint64_t now, std::uint64_t ticksPerSecond)
    {
        epoch = now;
        frequency = ticksPerSecond;
    }
    bool Contains(std::uint64_t timestamp) const
    {
        return frequency && timestamp >= epoch && (timestamp - epoch) / frequency < 3;
    }
    bool Finished(std::uint64_t now) const
    {
        return frequency && now >= epoch && (now - epoch) / frequency >= 3;
    }
};
struct Moments
{
    std::uint64_t n = 0;
    double mean = 0, m2 = 0, minimum = 0, maximum = 0;
    void Add(double x)
    {
        if (!std::isfinite(x))
            return;
        if (!n)
            minimum = maximum = x;
        minimum = std::min(minimum, x);
        maximum = std::max(maximum, x);
        ++n;
        const double d = x - mean;
        mean += d / static_cast<double>(n);
        m2 += d * (x - mean);
    }
    double Noise() const
    {
        return n > 1 ? std::sqrt(std::max(0.0, m2 / static_cast<double>(n - 1))) : 0;
    }
};
struct Axis
{
    double current = 0;
    Moments range, neutral;
};
struct Device
{
    std::string label, family;
    std::uint64_t reports = 0, first = 0, last = 0, invalid_decode = 0, out_of_order = 0;
    Moments cadence;
    std::map<std::string, Axis> axes;
    std::set<std::string> pressed, covered, known_buttons;
    void Report(std::uint64_t timestamp, double frequency)
    {
        ++reports;
        if (!first)
            first = timestamp;
        if (last && timestamp > last && frequency > 0)
            cadence.Add(static_cast<double>(timestamp - last) * 1000.0 / frequency);
        else if (last && timestamp <= last)
            ++out_of_order;
        if (timestamp > last)
            last = timestamp;
    }
    void Value(const std::string &key, double value, bool neutral)
    {
        if (!std::isfinite(value) || axes.size() >= 256 && axes.find(key) == axes.end())
            return;
        auto &a = axes[key];
        a.current = value;
        a.range.Add(value);
        if (neutral)
            a.neutral.Add(value);
    }
    void Button(const std::string &key, bool down)
    {
        if (known_buttons.size() >= 512 && known_buttons.find(key) == known_buttons.end())
            return;
        known_buttons.insert(key);
        if (down)
        {
            pressed.insert(key);
            covered.insert(key);
        }
        else
            pressed.erase(key);
    }
};
inline std::string Escape(const std::string &in)
{
    std::string r;
    for (char c : in)
        switch (c)
        {
        case '&':
            r += "&amp;";
            break;
        case '<':
            r += "&lt;";
            break;
        case '>':
            r += "&gt;";
            break;
        case '"':
            r += "&quot;";
            break;
        case '\'':
            r += "&#39;";
            break;
        default:
            r += c;
        }
    return r;
}
inline std::string Csv(const std::string &in)
{
    std::string r = "\"";
    for (char c : in)
    {
        if (c == '"')
            r += '"';
        r += c;
    }
    return r + '"';
}
inline std::string CsvSafe(const std::string &in)
{
    const auto first = in.find_first_not_of(" \t\r\n");
    if (first != std::string::npos &&
        (in[first] == '=' || in[first] == '+' || in[first] == '-' || in[first] == '@'))
        return Csv("'" + in);
    return Csv(in);
}
inline std::string Report(const std::map<std::string, Device> &devices, const std::string &job, bool html)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(5);
    if (html)
        out << "<!doctype html><html lang=en><meta charset=utf-8><title>Controller Repair "
               "Bench</title><style>body{font:16px system-ui;max-width:1100px;margin:40px "
               "auto;color:#182034}table{border-collapse:collapse;width:100%}td,th{padding:10px;border-bottom:1px "
               "solid #ddd;text-align:left}small{color:#456}</style><h1>Controller Repair Bench</h1><p>Job / "
               "serial: "
            << Escape(job)
            << "</p><p>Observed input reports, not a calibration certificate or physical end-to-end latency "
               "measurement. Neutral metrics use the fixed three-second window after capture initializes or a reset is acknowledged; leave sticks untouched while sampling. Late first reports are not sampled as neutral. HID "
               "fields retain report/collection identity.</p>";
    else
        out << "job,device,family,field,current,min,max,neutral_mean,neutral_noise,reports,mean_interval_ms,"
               "covered_buttons,observed_buttons\n";
    for (const auto &pair : devices)
    {
        const auto &d = pair.second;
        if (html)
            out << "<h2>" << Escape(d.label) << "</h2><p>" << Escape(d.family) << " | Reports: " << d.reports
                << " | Mean source interval: " << d.cadence.mean << " ms | Covered buttons: " << d.covered.size()
                << " / " << d.known_buttons.size() << " observed | Incomplete HID decodes: " << d.invalid_decode
                << " | Non-increasing timestamps: " << d.out_of_order
                << "</p><table><tr><th>Field</th><th>Current</th><th>Min</th><th>Max</th><th>Neutral "
                   "mean</th><th>Neutral noise (sample SD)</th></tr>";
        for (const auto &axis : d.axes)
        {
            const auto &a = axis.second;
            if (html)
                out << "<tr><td>" << Escape(axis.first) << "</td><td>" << a.current << "</td><td>"
                    << a.range.minimum << "</td><td>" << a.range.maximum << "</td><td>"
                    << (a.neutral.n ? std::to_string(a.neutral.mean) : "not sampled") << "</td><td>"
                    << (a.neutral.n > 1 ? std::to_string(a.neutral.Noise()) : "not sampled") << "</td></tr>";
            else
                out << CsvSafe(job) << ',' << CsvSafe(d.label) << ',' << CsvSafe(d.family) << ','
                    << CsvSafe(axis.first) << ',' << a.current << ',' << a.range.minimum << ',' << a.range.maximum
                    << ',' << (a.neutral.n ? std::to_string(a.neutral.mean) : "") << ','
                    << (a.neutral.n > 1 ? std::to_string(a.neutral.Noise()) : "") << ',' << d.reports << ','
                    << d.cadence.mean << ',' << d.covered.size() << ',' << d.known_buttons.size() << '\n';
        }
        if (html)
            out << "</table>";
        if (html)
            out << "<h3>Observed button fields</h3><ul>";
        for (const auto &button : d.known_buttons)
        {
            if (html)
                out << "<li>" << Escape(button) << ": "
                    << (d.covered.count(button) ? "exercised" : "not exercised") << "</li>";
            else
                out << CsvSafe(job) << ',' << CsvSafe(d.label) << ',' << CsvSafe(d.family) << ','
                    << CsvSafe("Button " + button) << ',' << (d.pressed.count(button) ? 1 : 0) << ",,,,,"
                    << d.reports << ',' << d.cadence.mean << ',' << d.covered.size() << ','
                    << d.known_buttons.size() << '\n';
        }
        if (html)
            out << "</ul>";
    }
    if (html)
        out << "</html>";
    return out.str();
}
} // namespace Bench
