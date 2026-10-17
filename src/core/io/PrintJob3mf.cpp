// SPDX-FileCopyrightText: Elias Högbom Aronsson and Loböle contributors
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "core/io/PrintJob3mf.h"

#include <array>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

// The deflate the screenshots use, compiled here with its functions static
// so the two copies do not meet at link time.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "thirdparty/stb_image_write.h"
#pragma GCC diagnostic pop

namespace printjob {

namespace {

constexpr const char* kCoreNs =
    "http://schemas.microsoft.com/3dmanufacturing/core/2015/02";
constexpr const char* kBambuNs = "http://schemas.bambulab.com/package/2021";
// The Bambu Studio this format was measured against; the project form says
// it so Bambu reads its plates and config.
constexpr const char* kBambuApplication = "BambuStudio-02.08.00.50";

std::string Escape(const std::string& s) {
  std::string o;
  o.reserve(s.size());
  for (char c : s) {
    switch (c) {
    case '&':
      o += "&amp;";
      break;
    case '<':
      o += "&lt;";
      break;
    case '>':
      o += "&gt;";
      break;
    case '"':
      o += "&quot;";
      break;
    case '\'':
      o += "&apos;";
      break;
    default:
      o += c;
    }
  }
  return o;
}

void Num(std::string& out, double v) {
  // Shortest float round trip: a mesh is float data (docs/conventions.md#
  // the-model-is-double), and nine digits is a tenth of a micron on a bed.
  char buf[32];
  const float f = static_cast<float>(v);
  const auto r = std::to_chars(buf, buf + sizeof(buf), f);
  out.append(buf, r.ptr);
}

std::string Fixed(double v) {
  char buf[32];
  const auto r = std::to_chars(buf, buf + sizeof(buf), v);
  return std::string(buf, r.ptr);
}

std::string ModelXml(const Job& job) {
  std::string x;
  x += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  x += std::string("<model unit=\"millimeter\" xml:lang=\"en-US\" xmlns=\"") +
       kCoreNs + "\" xmlns:BambuStudio=\"" + kBambuNs + "\">\n";
  if (job.projectConfig)
    x += std::string(" <metadata name=\"Application\">") + kBambuApplication +
         "</metadata>\n <metadata name=\"BambuStudio:3mfVersion\">1</"
         "metadata>\n";
  x += " <metadata name=\"Title\">" +
       Escape(job.record.value("name", std::string("print job"))) +
       "</metadata>\n";
  x += " <resources>\n";
  int id = 0;
  for (const Plate& plate : job.plates)
    for (const Object& o : plate.objects) {
      x += "  <object id=\"" + std::to_string(++id) +
           "\" type=\"model\" name=\"" + Escape(o.name) +
           "\">\n   <mesh>\n    <vertices>\n";
      for (const glm::dvec3& v : o.vertices) {
        x += "     <vertex x=\"";
        Num(x, v.x);
        x += "\" y=\"";
        Num(x, v.y);
        x += "\" z=\"";
        Num(x, v.z);
        x += "\"/>\n";
      }
      x += "    </vertices>\n    <triangles>\n";
      for (const auto& t : o.triangles)
        x += "     <triangle v1=\"" + std::to_string(t[0]) + "\" v2=\"" +
             std::to_string(t[1]) + "\" v3=\"" + std::to_string(t[2]) +
             "\"/>\n";
      x += "    </triangles>\n   </mesh>\n  </object>\n";
    }
  x += " </resources>\n <build>\n";
  id = 0;
  const int n = static_cast<int>(job.plates.size());
  for (int k = 0; k < n; k++) {
    const glm::dvec2 at = PlateOrigin(k, n, job.bedMm);
    for (size_t i = 0; i < job.plates[k].objects.size(); i++)
      x += "  <item objectid=\"" + std::to_string(++id) +
           "\" transform=\"1 0 0 0 1 0 0 0 1 " + Fixed(at.x) + " " +
           Fixed(at.y) + " 0\" printable=\"1\"/>\n";
  }
  x += " </build>\n</model>\n";
  return x;
}

std::string ModelSettingsXml(const Job& job) {
  std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<config>\n";
  const auto meta = [&](const std::string& indent, const std::string& key,
                        const std::string& value) {
    x += indent + "<metadata key=\"" + Escape(key) + "\" value=\"" +
         Escape(value) + "\"/>\n";
  };
  int id = 0;
  for (const Plate& plate : job.plates)
    for (const Object& o : plate.objects) {
      ++id;
      x += "  <object id=\"" + std::to_string(id) + "\">\n";
      meta("    ", "name", o.name);
      meta("    ", "extruder", "1");
      for (const auto& [k, v] : o.settings)
        meta("    ", k, v);
      x += "    <part id=\"" + std::to_string(id) +
           "\" subtype=\"normal_part\">\n";
      meta("      ", "name", o.name);
      meta("      ", "matrix", "1 0 0 0 0 1 0 0 0 0 1 0 0 0 0 1");
      x += "    </part>\n  </object>\n";
    }
  id = 0;
  for (size_t k = 0; k < job.plates.size(); k++) {
    x += "  <plate>\n";
    meta("    ", "plater_id", std::to_string(k + 1));
    meta("    ", "plater_name", job.plates[k].name);
    meta("    ", "locked", "false");
    for (size_t i = 0; i < job.plates[k].objects.size(); i++) {
      ++id;
      x += "    <model_instance>\n";
      meta("      ", "object_id", std::to_string(id));
      meta("      ", "instance_id", "0");
      // The label the G-code carries the object under.
      meta("      ", "identify_id", std::to_string(100 + id));
      x += "    </model_instance>\n";
    }
    x += "  </plate>\n";
  }
  x += "</config>\n";
  return x;
}

uint32_t Crc32(const std::string& s) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++)
        c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = 0xFFFFFFFFu;
  for (unsigned char b : s)
    c = table[(c ^ b) & 0xFFu] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

void Put16(std::string& o, uint32_t v) {
  o += static_cast<char>(v & 0xFF);
  o += static_cast<char>((v >> 8) & 0xFF);
}
void Put32(std::string& o, uint32_t v) {
  Put16(o, v & 0xFFFF);
  Put16(o, v >> 16);
}

// Raw deflate of `s` (stb writes a zlib stream: two bytes of header and an
// Adler-32 trailer around it).
std::string Deflate(const std::string& s) {
  if (s.size() > static_cast<size_t>(std::numeric_limits<int>::max() / 2))
    throw std::runtime_error("print job: an entry over 1 GB will not zip");
  int len = 0;
  unsigned char* z = stbi_zlib_compress(
      reinterpret_cast<unsigned char*>(const_cast<char*>(s.data())),
      static_cast<int>(s.size()), &len, 5);
  if (z == nullptr || len < 6) {
    STBIW_FREE(z);
    throw std::runtime_error("print job: deflate failed");
  }
  std::string out(reinterpret_cast<const char*>(z) + 2,
                  static_cast<size_t>(len) - 6);
  STBIW_FREE(z);
  return out;
}

} // namespace

glm::dvec2 PlateOrigin(int index, int count, const glm::dvec2& bedMm) {
  // Bambu Studio's PartPlateList: sqrt(n) columns, one more when it
  // rounds down; each plate a bed and a fifth on from the last, rows going
  // toward -y.
  const double root = std::sqrt(static_cast<double>(std::max(count, 1)));
  const double rounded = std::round(root);
  const int cols = static_cast<int>(root > rounded ? rounded + 1 : rounded);
  const int col = index % std::max(cols, 1), row = index / std::max(cols, 1);
  constexpr double kGap = 1.0 / 5.0;
  // (+ 0.0: the first row is at 0, not -0, which a transform would print.)
  return {col * bedMm.x * (1.0 + kGap), -row * bedMm.y * (1.0 + kGap) + 0.0};
}

std::vector<std::pair<std::string, std::string>> Entries(const Job& job) {
  std::vector<std::pair<std::string, std::string>> e;
  e.emplace_back(
      "[Content_Types].xml",
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Types "
      "xmlns=\"http://schemas.openxmlformats.org/package/2006/"
      "content-types\">\n <Default Extension=\"rels\" "
      "ContentType=\"application/"
      "vnd.openxmlformats-package.relationships+xml\"/>\n <Default "
      "Extension=\"model\" "
      "ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/"
      ">\n</Types>\n");
  e.emplace_back(
      "_rels/.rels",
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Relationships "
      "xmlns=\"http://schemas.openxmlformats.org/package/2006/"
      "relationships\">\n <Relationship Target=\"/3D/3dmodel.model\" "
      "Id=\"rel-1\" "
      "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/"
      "3dmodel\"/>\n</Relationships>\n");
  e.emplace_back("3D/3dmodel.model", ModelXml(job));
  e.emplace_back("Metadata/model_settings.config", ModelSettingsXml(job));
  if (job.projectConfig)
    e.emplace_back("Metadata/project_settings.config",
                   job.projectConfig->dump(4) + "\n");
  e.emplace_back("Metadata/lobole_job.json", job.record.dump(2) + "\n");
  return e;
}

std::string Archive(const Job& job) {
  std::string zip, central;
  uint16_t count = 0;
  // 1980-01-01 00:00, the zip epoch: the same job zips to the same bytes.
  constexpr uint32_t kTime = 0, kDate = (0 << 9) | (1 << 5) | 1;
  for (const auto& [name, data] : Entries(job)) {
    const uint32_t crc = Crc32(data);
    const std::string z = Deflate(data);
    const uint32_t offset = static_cast<uint32_t>(zip.size());
    if (zip.size() + z.size() > 0xFFFFFFF0ull || data.size() > 0xFFFFFFF0ull)
      throw std::runtime_error("print job: over 4 GB, past what a zip "
                               "without zip64 holds");
    Put32(zip, 0x04034b50u);
    Put16(zip, 20);
    Put16(zip, 0);
    Put16(zip, 8);
    Put16(zip, kTime);
    Put16(zip, kDate);
    Put32(zip, crc);
    Put32(zip, static_cast<uint32_t>(z.size()));
    Put32(zip, static_cast<uint32_t>(data.size()));
    Put16(zip, static_cast<uint32_t>(name.size()));
    Put16(zip, 0);
    zip += name;
    zip += z;

    Put32(central, 0x02014b50u);
    Put16(central, 20);
    Put16(central, 20);
    Put16(central, 0);
    Put16(central, 8);
    Put16(central, kTime);
    Put16(central, kDate);
    Put32(central, crc);
    Put32(central, static_cast<uint32_t>(z.size()));
    Put32(central, static_cast<uint32_t>(data.size()));
    Put16(central, static_cast<uint32_t>(name.size()));
    Put16(central, 0);
    Put16(central, 0);
    Put16(central, 0);
    Put16(central, 0);
    Put32(central, 0);
    Put32(central, offset);
    central += name;
    count++;
  }
  const uint32_t cdOffset = static_cast<uint32_t>(zip.size());
  zip += central;
  Put32(zip, 0x06054b50u);
  Put16(zip, 0);
  Put16(zip, 0);
  Put16(zip, count);
  Put16(zip, count);
  Put32(zip, static_cast<uint32_t>(central.size()));
  Put32(zip, cdOffset);
  Put16(zip, 0);
  return zip;
}

std::string MissingProjectKeys(const nlohmann::json& config) {
  if (!config.is_object())
    return "a project config is a JSON object";
  // What Bambu Studio's CLI will not load a project without (measured:
  // drop any one and it crashes), plus what the job checks it against.
  static const char* const kKeys[] = {
      "printer_settings_id", "print_settings_id", "filament_settings_id",
      "nozzle_diameter",     "printable_height",  "printable_area",
      "filament_type",       "layer_height"};
  std::string missing;
  for (const char* k : kKeys)
    if (!config.contains(k))
      missing += (missing.empty() ? "" : ", ") + std::string(k);
  return missing;
}

} // namespace printjob
