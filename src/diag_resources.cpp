// Diagnostic: dump all image resources from a PSD file
#include "binary_reader.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: diag_resources <file.psd>\n");
        return 1;
    }

    try {
        BinaryReader r(argv[1]);

        // Header
        uint8_t sig[4];
        r.read_bytes(sig, 4);
        uint16_t version = r.read_u16();
        r.skip(6);
        uint16_t channels = r.read_u16();
        uint32_t height = r.read_u32();
        uint32_t width = r.read_u32();
        uint16_t depth = r.read_u16();
        uint16_t mode = r.read_u16();

        printf("Header: %ux%u, %u channels, depth=%u, mode=%u\n\n", width, height, channels, depth, mode);

        // Color Mode Data
        uint32_t cmd_len = r.read_u32();
        r.skip(cmd_len);

        // Image Resources
        uint32_t section_len = r.read_u32();
        size_t section_end = r.position() + section_len;

        printf("Image Resources section: %u bytes\n", section_len);
        printf("========================================\n\n");

        int res_count = 0;
        while (r.position() < section_end) {
            uint8_t sig[4];
            r.read_bytes(sig, 4);
            if (sig[0] != '8' || sig[1] != 'B' || sig[2] != 'I' || sig[3] != 'M') {
                printf("BAD SIGNATURE at offset %zu\n", r.position() - 4);
                break;
            }

            uint16_t resource_id = r.read_u16();
            std::string name = r.read_pascal_string();
            uint32_t data_len = r.read_u32();
            size_t data_start = r.position();

            printf("Resource %d (0x%04X), name='%s', size=%u",
                   resource_id, resource_id, name.c_str(), data_len);

            // Dump interesting resources in detail
            if (resource_id == 1039) {
                printf(" [ICC Profile]\n");
            } else if (resource_id == 1045) {
                printf(" [Unicode Alpha Names]\n");
                // Parse and show channel names
                auto raw = r.read_vec(data_len);
                size_t off = 0;
                int idx = 0;
                while (off + 4 <= raw.size()) {
                    uint32_t char_count = (uint32_t(raw[off]) << 24) | (uint32_t(raw[off+1]) << 16) |
                                          (uint32_t(raw[off+2]) << 8) | raw[off+3];
                    off += 4;
                    if (char_count == 0 || off + char_count * 2 > raw.size()) break;
                    std::string n;
                    for (uint32_t i = 0; i < char_count; ++i) {
                        uint16_t ch = (uint16_t(raw[off]) << 8) | raw[off+1];
                        off += 2;
                        if (ch == 0) break;
                        if (ch < 0x80) n += static_cast<char>(ch);
                        else if (ch < 0x800) { n += static_cast<char>(0xC0 | (ch >> 6)); n += static_cast<char>(0x80 | (ch & 0x3F)); }
                        else { n += static_cast<char>(0xE0 | (ch >> 12)); n += static_cast<char>(0x80 | ((ch >> 6) & 0x3F)); n += static_cast<char>(0x80 | (ch & 0x3F)); }
                    }
                    printf("  [%d] '%s'\n", idx, n.c_str());
                    idx++;
                }
            } else if (resource_id == 1046) {
                printf(" [Spot Color - ALTERNATE COLORS]\n");
                // Parse resource 1046: per-entry color info
                auto raw = r.read_vec(data_len);
                printf("  Raw hex (first 200 bytes): ");
                for (size_t i = 0; i < raw.size() && i < 200; ++i) {
                    printf("%02X ", raw[i]);
                    if ((i + 1) % 16 == 0) printf("\n  ");
                }
                printf("\n");

                // Try to parse: 2 bytes version, 2 bytes count, then entries
                if (raw.size() >= 4) {
                    uint16_t ver = (uint16_t(raw[0]) << 8) | raw[1];
                    uint16_t count = (uint16_t(raw[2]) << 8) | raw[3];
                    printf("  version=%u count=%u\n", ver, count);
                    size_t off = 4;
                    for (uint16_t i = 0; i < count && off + 14 <= raw.size(); ++i) {
                        int32_t ch_id = (int32_t((uint32_t(raw[off]) << 24) | (uint32_t(raw[off+1]) << 16) |
                                              (uint32_t(raw[off+2]) << 8) | raw[off+3]));
                        off += 4;
                        uint16_t cs = (uint16_t(raw[off]) << 8) | raw[off+1];
                        off += 2;
                        int16_t comp[4];
                        for (int j = 0; j < 4; ++j) {
                            comp[j] = int16_t((uint16_t(raw[off]) << 8) | raw[off+1]);
                            off += 2;
                        }
                        printf("  entry %d: channel_id=%d colorspace=%u components=[%d,%d,%d,%d]\n",
                               i, ch_id, cs, comp[0], comp[1], comp[2], comp[3]);
                    }
                }
            } else if (resource_id == 1067) {
                printf(" [Alternate Spot Colors]\n");
                auto raw = r.read_vec(data_len);
                printf("  Raw hex (first 200 bytes): ");
                for (size_t i = 0; i < raw.size() && i < 200; ++i) {
                    printf("%02X ", raw[i]);
                    if ((i + 1) % 16 == 0) printf("\n  ");
                }
                printf("\n");

                if (raw.size() >= 4) {
                    uint16_t ver = (uint16_t(raw[0]) << 8) | raw[1];
                    uint16_t count = (uint16_t(raw[2]) << 8) | raw[3];
                    printf("  version=%u count=%u\n", ver, count);
                    size_t off = 4;
                    for (uint16_t i = 0; i < count && off + 14 <= raw.size(); ++i) {
                        int32_t ch_id = (int32_t((uint32_t(raw[off]) << 24) | (uint32_t(raw[off+1]) << 16) |
                                              (uint32_t(raw[off+2]) << 8) | raw[off+3]));
                        off += 4;
                        uint16_t cs = (uint16_t(raw[off]) << 8) | raw[off+1];
                        off += 2;
                        int16_t comp[4];
                        for (int j = 0; j < 4; ++j) {
                            comp[j] = int16_t((uint16_t(raw[off]) << 8) | raw[off+1]);
                            off += 2;
                        }
                        printf("  entry %d: channel_id=%d colorspace=%u components=[%d,%d,%d,%d]\n",
                               i, ch_id, cs, comp[0], comp[1], comp[2], comp[3]);
                    }
                }
            } else if (resource_id == 1077) {
                printf(" [DisplayInfo]\n");
                auto raw = r.read_vec(data_len);
                printf("  Raw hex (first 200 bytes): ");
                for (size_t i = 0; i < raw.size() && i < 200; ++i) {
                    printf("%02X ", raw[i]);
                    if ((i + 1) % 16 == 0) printf("\n  ");
                }
                printf("\n");

                if (raw.size() >= 2) {
                    uint16_t count = (uint16_t(raw[0]) << 8) | raw[1];
                    printf("  count=%u\n", count);
                    size_t off = 2;
                    for (uint16_t i = 0; i < count && off + 12 <= raw.size(); ++i) {
                        uint16_t cs = (uint16_t(raw[off]) << 8) | raw[off+1];
                        off += 2;
                        int16_t comp[4];
                        for (int j = 0; j < 4; ++j) {
                            comp[j] = int16_t((uint16_t(raw[off]) << 8) | raw[off+1]);
                            off += 2;
                        }
                        off += 2; // padding
                        printf("  entry %d: colorspace=%u components=[%d,%d,%d,%d]\n",
                               i, cs, comp[0], comp[1], comp[2], comp[3]);
                    }
                }
            } else if (resource_id == 1053 || resource_id == 1054 || resource_id == 1064 ||
                       resource_id == 1073 || resource_id == 1076 || resource_id == 1080 ||
                       resource_id == 1082 || resource_id == 1083 || resource_id == 1084 ||
                       resource_id == 1085 || resource_id == 1086 || resource_id == 1088 ||
                       resource_id == 1093 || resource_id == 1094 || resource_id == 1095 ||
                       resource_id == 1096 || resource_id == 1097 || resource_id == 1098 ||
                       resource_id == 1099 || resource_id == 1100 || resource_id == 1101 ||
                       resource_id == 1102 || resource_id == 1103 || resource_id == 1108 ||
                       resource_id == 1109 || resource_id == 1110 || resource_id == 1111 ||
                       resource_id == 1112 || resource_id == 1113 || resource_id == 1114 ||
                       resource_id == 1115 || resource_id == 1116 || resource_id == 1117 ||
                       resource_id == 1118 || resource_id == 1119 || resource_id == 1120 ||
                       resource_id == 1121 || resource_id == 1122 || resource_id == 1123 ||
                       resource_id == 1124 || resource_id == 1125 || resource_id == 1126 ||
                       resource_id == 1127 || resource_id == 1128 || resource_id == 1129 ||
                       resource_id == 1130 || resource_id == 1131 || resource_id == 1132 ||
                       resource_id == 1133 || resource_id == 1134 || resource_id == 1135 ||
                       resource_id == 1136 || resource_id == 1137 || resource_id == 1138 ||
                       resource_id == 1139 || resource_id == 1140 || resource_id == 1141 ||
                       resource_id == 1142 || resource_id == 1143 || resource_id == 1144 ||
                       resource_id == 1145 || resource_id == 1146 || resource_id == 1147 ||
                       resource_id == 1148 || resource_id == 1149 || resource_id == 1150 ||
                       resource_id == 1151 || resource_id == 1152 || resource_id == 1153 ||
                       resource_id == 1154 || resource_id == 1155 || resource_id == 1156 ||
                       resource_id == 1157 || resource_id == 1158 || resource_id == 1159 ||
                       resource_id == 1160 || resource_id == 1161 || resource_id == 1162 ||
                       resource_id == 1163 || resource_id == 1164 || resource_id == 1165 ||
                       resource_id == 1166 || resource_id == 1167 || resource_id == 1168 ||
                       resource_id == 1169 || resource_id == 1170 || resource_id == 1171 ||
                       resource_id == 1172 || resource_id == 1173 || resource_id == 1174 ||
                       resource_id == 1175 || resource_id == 1176 || resource_id == 1177 ||
                       resource_id == 1178 || resource_id == 1179 || resource_id == 1180 ||
                       resource_id == 1181 || resource_id == 1182 || resource_id == 1183 ||
                       resource_id == 1184 || resource_id == 1185 || resource_id == 1186 ||
                       resource_id == 1187 || resource_id == 1188 || resource_id == 1189 ||
                       resource_id == 1190 || resource_id == 1191 || resource_id == 1192 ||
                       resource_id == 1193 || resource_id == 1194 || resource_id == 1195 ||
                       resource_id == 1196 || resource_id == 1197 || resource_id == 1198 ||
                       resource_id == 1199 || resource_id == 1200 || resource_id == 1201 ||
                       resource_id == 1202 || resource_id == 1203 || resource_id == 1204 ||
                       resource_id == 1205 || resource_id == 1206 || resource_id == 1207 ||
                       resource_id == 1208 || resource_id == 1209 || resource_id == 1210 ||
                       resource_id == 1211 || resource_id == 1212 || resource_id == 1213 ||
                       resource_id == 1214 || resource_id == 1215 || resource_id == 1216 ||
                       resource_id == 1217 || resource_id == 1218 || resource_id == 1219 ||
                       resource_id == 1220 || resource_id == 1221 || resource_id == 1222 ||
                       resource_id == 1223 || resource_id == 1224 || resource_id == 1225 ||
                       resource_id == 1226 || resource_id == 1227 || resource_id == 1228 ||
                       resource_id == 1229 || resource_id == 1230 || resource_id == 1231 ||
                       resource_id == 1232 || resource_id == 1233 || resource_id == 1234 ||
                       resource_id == 1235 || resource_id == 1236 || resource_id == 1237 ||
                       resource_id == 1238 || resource_id == 1239 || resource_id == 1240 ||
                       resource_id == 1241 || resource_id == 1242 || resource_id == 1243 ||
                       resource_id == 1244 || resource_id == 1245 || resource_id == 1246 ||
                       resource_id == 1247 || resource_id == 1248 || resource_id == 1249 ||
                       resource_id == 1250 || resource_id == 1251 || resource_id == 1252 ||
                       resource_id == 1253 || resource_id == 1254 || resource_id == 1255 ||
                       resource_id == 1256 || resource_id == 1257 || resource_id == 1258 ||
                       resource_id == 1259 || resource_id == 1260 || resource_id == 1261 ||
                       resource_id == 1262 || resource_id == 1263 || resource_id == 1264 ||
                       resource_id == 1265 || resource_id == 1266 || resource_id == 1267 ||
                       resource_id == 1268 || resource_id == 1269 || resource_id == 1270 ||
                       resource_id == 1271 || resource_id == 1272 || resource_id == 1273 ||
                       resource_id == 1274 || resource_id == 1275 || resource_id == 1276 ||
                       resource_id == 1277 || resource_id == 1278 || resource_id == 1279 ||
                       resource_id == 1280 || resource_id == 1281 || resource_id == 1282 ||
                       resource_id == 1283 || resource_id == 1284 || resource_id == 1285 ||
                       resource_id == 1286 || resource_id == 1287 || resource_id == 1288 ||
                       resource_id == 1289 || resource_id == 1290 || resource_id == 1291 ||
                       resource_id == 1292 || resource_id == 1293 || resource_id == 1294 ||
                       resource_id == 1295 || resource_id == 1296 || resource_id == 1297 ||
                       resource_id == 1298 || resource_id == 1299 || resource_id == 3000 ||
                       resource_id == 3001 || resource_id == 3002 || resource_id == 3003 ||
                       resource_id == 3004 || resource_id == 4000 || resource_id == 4001 ||
                       resource_id == 4002 || resource_id == 4003 || resource_id == 4004 ||
                       resource_id == 4005 || resource_id == 4006 || resource_id == 4007 ||
                       resource_id == 4008 || resource_id == 4009 || resource_id == 4010 ||
                       resource_id == 4011 || resource_id == 4012 || resource_id == 4013 ||
                       resource_id == 4014 || resource_id == 4015 || resource_id == 4016 ||
                       resource_id == 4017 || resource_id == 4018 || resource_id == 4019 ||
                       resource_id == 4020 || resource_id == 4021 || resource_id == 4022 ||
                       resource_id == 4023 || resource_id == 4024 || resource_id == 4025 ||
                       resource_id == 4026 || resource_id == 4027 || resource_id == 4028 ||
                       resource_id == 4029 || resource_id == 4030 || resource_id == 4031 ||
                       resource_id == 4032 || resource_id == 4033 || resource_id == 4034 ||
                       resource_id == 4035 || resource_id == 4036 || resource_id == 4037 ||
                       resource_id == 4038 || resource_id == 4039 || resource_id == 4040 ||
                       resource_id == 4041 || resource_id == 4042 || resource_id == 4043 ||
                       resource_id == 4044 || resource_id == 4045 || resource_id == 4046 ||
                       resource_id == 4047 || resource_id == 4048 || resource_id == 4049 ||
                       resource_id == 4050 || resource_id == 4051 || resource_id == 4052 ||
                       resource_id == 4053 || resource_id == 4054 || resource_id == 4055 ||
                       resource_id == 4056 || resource_id == 4057 || resource_id == 4058 ||
                       resource_id == 4059 || resource_id == 4060 || resource_id == 4061 ||
                       resource_id == 4062 || resource_id == 4063 || resource_id == 4064 ||
                       resource_id == 4065 || resource_id == 4066 || resource_id == 4067 ||
                       resource_id == 4068 || resource_id == 4069 || resource_id == 4070 ||
                       resource_id == 4071 || resource_id == 4072 || resource_id == 4073 ||
                       resource_id == 4074 || resource_id == 4075 || resource_id == 4076 ||
                       resource_id == 4077 || resource_id == 4078 || resource_id == 4079 ||
                       resource_id == 4080 || resource_id == 4081 || resource_id == 4082 ||
                       resource_id == 4083 || resource_id == 4084 || resource_id == 4085 ||
                       resource_id == 4086 || resource_id == 4087 || resource_id == 4088 ||
                       resource_id == 4089 || resource_id == 4090 || resource_id == 4091 ||
                       resource_id == 4092 || resource_id == 4093 || resource_id == 4094 ||
                       resource_id == 4095 || resource_id == 4096 || resource_id == 4097 ||
                       resource_id == 4098 || resource_id == 4099 || resource_id == 4100 ||
                       resource_id == 4101 || resource_id == 4102 || resource_id == 4103 ||
                       resource_id == 4104 || resource_id == 4105 || resource_id == 4106 ||
                       resource_id == 4107 || resource_id == 4108 || resource_id == 4109 ||
                       resource_id == 4110 || resource_id == 4111 || resource_id == 4112 ||
                       resource_id == 4113 || resource_id == 4114 || resource_id == 4115 ||
                       resource_id == 4116 || resource_id == 4117 || resource_id == 4118 ||
                       resource_id == 4119 || resource_id == 4120) {
                printf(" [POTENTIALLY RELEVANT]\n");
                r.skip(data_len);
            } else {
                printf("\n");
                r.skip(data_len);
            }

            // Pad to even
            if (data_len % 2 != 0) r.skip(1);

            res_count++;
        }

        printf("\nTotal resources: %d\n", res_count);

        if (r.position() != section_end) {
            printf("WARNING: position %zu != expected end %zu (diff=%zd)\n",
                   r.position(), section_end, (ssize_t)(section_end - r.position()));
        }

    } catch (const std::exception& e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
    return 0;
}
