# M3 — 실보드 브링업: Waveshare ESP32-S3-Touch-LCD-3.5B (xiaozhi-esp32)

**작성:** embedded-engineer (도하람) · 2026-06-30
**대상 보드:** AliExpress 1005011936539776 (Spotpear) = **Waveshare `ESP32-S3-Touch-LCD-3.5B` 동등품**
**관련 이슈:** ALI-21 (M3)

---

## TL;DR (결론)

1. **xiaozhi-esp32 공식 펌웨어가 이 보드를 이미 지원한다.** 보드 프로파일
   `main/boards/waveshare/esp32-s3-touch-lcd-3.5b` 가 존재하고, **카메라(OV5640+OV2640)·
   디스플레이(AXS15231B QSPI)·오디오 코덱(ES8311)·터치 핀이 전부 정의돼 있다.**
   → **핀포팅 불필요. 1순위 전략 채택.**
2. **카메라/비전은 이 펌웨어로 해결된다.** 보드 빌드에 `CONFIG_CAMERA_OV5640=y` /
   `CONFIG_CAMERA_OV2640=y` 가 기본 포함. 별도 카메라 캡처 코드를 0부터 짤 필요 없음.
3. **빌드 시스템은 ESP-IDF다 (PlatformIO 아님).** 우리 저장소의 `firmware/`
   (PlatformIO + INMP441/MAX98357A DIY 보드)는 **이 보드에 쓰이지 않는다.**
   `pio run` 은 이 보드 검증에 해당사항 없음. 빌드는 `idf.py build`.
4. **이 보드의 오디오는 ES8311 I2C 코덱**이다. 우리 M0 `main.cpp` 의 INMP441/
   MAX98357A I2S 직결 코드와는 하드웨어가 다르다. M0 펌웨어는 손대지 않고 보존.
5. **결과적으로 ALI-20(커스텀 WebSocket 펌웨어)은 불필요할 가능성이 높다.**
   xiaozhi 공식 펌웨어가 WebSocket 오디오 + MCP + 카메라 비전을 전부 제공하므로,
   우리가 프로토콜을 새로 구현할 이유가 없다. → infra/embedded 조율 필요 (아래 §6).

> ⚠️ **빌드 통과 ≠ 실기 동작.** 본 문서는 소스 검증(보드 프로파일 존재·핀맵·
> 카메라 컨피그 확인)까지다. 실제 `idf.py build` 컴파일과 USB 플래싱·시리얼
> 검증은 **장비/ESP-IDF 환경 확보 후 기영님**이 수행한다 (§4, §5 절차 제공).

---

## 1. 보드 식별 — 어느 프로파일인가 (3.5 vs 3.5B 주의)

xiaozhi-esp32 `main/boards/waveshare/` 에는 비슷한 두 프로파일이 있다. **반드시 구분:**

| 프로파일 | 디스플레이 IC | 버스 | 우리 보드? |
|---|---|---|---|
| `esp32-s3-touch-lcd-3.5`  | ST7796 | SPI | ✗ |
| **`esp32-s3-touch-lcd-3.5b`** | **AXS15231B** | **QSPI** | ✅ **이것** |

근거: 구매한 보드의 AliExpress/Spotpear 리스팅이 **`AXS15231B … QSPI … N16R8`**
(QSPI 변형). 따라서 **B 변형 = `esp32-s3-touch-lcd-3.5b`** 가 정확한 타깃.

> 🔎 **실물 재확인 1건 (기영님):** 보드 실크에 `ESP32S3-3.5inch-AI V1.0` 과 함께
> 디스플레이 IC가 **AXS15231B** 인지 눈으로 확인. 만약 실크가 ST7796 이면 `-3.5b`
> 가 아니라 `-3.5` 프로파일을 써야 한다. 이슈 본문에도 "IC 변형 둘(AXS15231B/
> ST7796)" 경고가 있었음 — 이 한 줄이 빌드 타깃을 가른다.

### 확정 사양 (config 검증됨)
- SoC: ESP32-S3R8 (N16R8) — 8MB PSRAM / 16MB Flash ✅
- 디스플레이: AXS15231B, QSPI, 480×320 (코드상 가로 480×세로 320, 90° 회전)
- 카메라: OV5640(5MP) / OV2640 DVP, 자동 감지
- 오디오 코덱: **ES8311** (I2C 0x18 기본), I2S 24kHz in/out
- 터치: 정전식 (TOUCH_ENABLE 1)

---

## 2. 핀맵 (xiaozhi `esp32-s3-touch-lcd-3.5b/config.h` 에서 검증)

> 이 핀들은 **xiaozhi 펌웨어가 이미 정의**한 값이다. 우리가 바꾸지 않는다.
> 참고/디버깅·하드웨어 대조용으로 기록한다. (DIY 보드 핀맵은 `docs/hardware.md`
> 의 별도 표 — 둘은 다른 보드이므로 서로 무관.)

### 오디오 (ES8311 I2S + I2C)
| 기능 | GPIO |
|---|---:|
| I2S MCLK | 44 |
| I2S WS   | 15 |
| I2S BCLK | 13 |
| I2S DIN (mic→S3) | 14 |
| I2S DOUT (S3→spk) | 16 |
| Codec I2C SDA | 8 |
| Codec I2C SCL | 7 |
| PA enable | NC (없음) |

### 디스플레이 (AXS15231B QSPI)
| 기능 | GPIO |
|---|---:|
| CS | 12 |
| CLK | 5 |
| DATA0 | 1 |
| DATA1 | 2 |
| DATA2 | 3 |
| DATA3 | 4 |
| RST | NC |
| Backlight | 6 |

### 카메라 (DVP 8-bit, OV5640/OV2640)
| 기능 | GPIO |   | 기능 | GPIO |
|---|---:|---|---|---:|
| PWDN | NC | | D0 | 45 |
| RESET | NC | | D1 | 47 |
| VSYNC | 17 | | D2 | 48 |
| HREF | 18 | | D3 | 46 |
| PCLK | 41 | | D4 | 42 |
| XCLK | 38 | | D5 | 40 |
| SIOD (SCCB SDA) | NC* | | D6 | 39 |
| SIOC (SCCB SCL) | NC* | | D7 | 21 |

\* SCCB(SIOD/SIOC)가 `NC` 인 것은 **카메라 제어선이 오디오 코덱과 동일 I2C 버스
(SDA=8, SCL=7)를 공유**하기 때문. 카메라 드라이버가 그 I2C 버스로 센서를 제어한다.

### 버튼 / LED
| 기능 | GPIO |
|---|---:|
| BOOT 버튼 | 0 |
| 내장 LED | NC |
| 볼륨 ±  | NC |

---

## 3. 카메라 프레임 → 서버(vLLM 비전) → 한국어 설명 경로 설계

xiaozhi 스택은 비전을 **MCP(Model Context Protocol) 기반**으로 처리한다. 우리가
HTTP/JPEG 업로드 경로를 새로 짤 필요 없이, 아래 흐름이 펌웨어+서버에 내장돼 있다.

```text
사용자 음성: "이거 뭐야?" / "사진 찍어서 봐줘"
        │  (I2S mic, ES8311)
        ▼
ESP32-S3  ──WebSocket(오디오)──►  xiaozhi-esp32-server (ws://<4090>:8003/xiaozhi/v1/)
        │                              │
        │                              ▼  STT(SenseVoice) → LLM(Qwen3.6) 의도 파싱
        │                              │
        │  ◄──MCP: self.camera.take_photo──┘   (LLM이 카메라 도구 호출)
        ▼
OV5640 캡처(YUV422→JPEG)
        │
        └──HTTP POST(JPEG + 질문)──►  서버 비전(Explain) 엔드포인트
                                          │
                                          ▼  Qwen3.6-27B 멀티모달 (vLLM OpenAI /v1/chat/completions, 이미지+텍스트)
                                          │
                                          ▼  한국어 설명 텍스트
                                          ▼  EdgeTTS(ko-KR) → WAV/PCM
        ◄────────WebSocket(오디오 응답)──────┘
        ▼
ES8311 → 스피커 ("앞에 머그컵이 하나 있네요." 등)
```

### 핵심 포인트
- **펌웨어 측 신규 코드 0.** `self.camera.take_photo` MCP 도구와 비전 Explain
  업로드는 xiaozhi-esp32 빌트인. 카메라 컨피그(OV5640/OV2640)도 보드 빌드에 포함.
- **서버 측 작업은 infra(ALI-19 xiaozhi-server)** 담당: `config.yaml` 의 `VLLM`
  (비전) 프로바이더를 **우리 Qwen3.6-27B vLLM 엔드포인트**(OpenAI 호환,
  4090:8000)로 연결. LLM 과 VLLM 이 같은 멀티모달 모델을 가리키므로 단일 엔드포인트.
- **포트:** vLLM=8000(OpenAI), xiaozhi WebSocket=8003. (HANDOFF §3-5 일치.)
- **한국어 락:** 페르소나/시스템 프롬프트는 서버 `config.yaml` 에서. 펌웨어 무관.

### DoD 매핑
- [x] 정확한 보드 프로파일/핀맵 확정 — `esp32-s3-touch-lcd-3.5b` (§1, §2)
- [~] 펌웨어 빌드 — 프로파일·컨피그 소스 검증 완료. 실제 `idf.py build` 는
      ESP-IDF 환경에서 (§4). **빌드 통과는 장비/툴체인 확보 후 보고.**
- [x] 카메라→서버→한국어 경로 설계 — 본 §3
- [x] 플래시 절차 + 시리얼 검증 체크리스트 — §4, §5

---

## 4. 빌드 & 플래시 절차 (ESP-IDF — 기영님 수행)

> 이 보드는 **xiaozhi-esp32 (ESP-IDF)** 로 빌드한다. 우리 `firmware/` (PlatformIO)
> 가 아니다. 가장 빠른 길은 **공식 펌웨어 릴리스/플래시 도구**이고, 소스 빌드가
> 필요하면 ESP-IDF v5.x 가 있어야 한다.

### 경로 A — 공식 펌웨어 바로 플래시 (권장, 가장 빠름)
1. xiaozhi 펌웨어 릴리스/플래시 페이지에서 **보드: ESP32-S3-Touch-LCD-3.5B** 선택.
   (Waveshare wiki 의 xiaozhi 안내, 또는 https://github.com/78/xiaozhi-esp32 릴리스.)
2. USB-C 연결. 보드가 안 잡히면 **BOOT(GPIO0) 누른 채 리셋**으로 다운로드 모드 진입.
3. 웹/도구로 펌웨어 굽기 → 재부팅.
4. 단, 공식 빌드는 기본 서버가 xiaozhi 공식 클라우드일 수 있음 → **우리 4090 서버로
   바꾸려면 경로 B(소스 빌드)에서 서버 주소를 박거나, 기기 설정/OTA 로 변경.**

### 경로 B — 소스 빌드 (우리 4090 서버 연동 확실)
```bash
# 1. ESP-IDF v5.x 설치 (espressif 공식 installer) 후 export
. $HOME/esp/esp-idf/export.sh

# 2. 펌웨어 클론
git clone --recursive https://github.com/78/xiaozhi-esp32.git
cd xiaozhi-esp32

# 3. 타깃 = esp32s3
idf.py set-target esp32s3

# 4. 보드 선택: menuconfig → "Xiaozhi Assistant" → BOARD_TYPE
#    => esp32-s3-touch-lcd-3.5b  를 선택
idf.py menuconfig
#    (또는 scripts 의 보드 지정 빌드 흐름 사용. config.json builds.name = esp32-s3-touch-lcd-3.5b)

# 5. (선택) 서버 주소: 펌웨어 OTA/설정 또는 보드 코드의 기본 ws URL을
#    ws://<4090-IP>:8003/xiaozhi/v1/ 로 지정

# 6. 빌드
idf.py build          # ← "빌드 통과" = 여기 성공

# 7. 플래시 + 모니터 (포트는 환경에 맞게)
idf.py -p COM<x> flash monitor
```

> **빌드 통과 판정:** `idf.py build` 가 `Project build complete` 로 끝나고
> `build/xiaozhi.bin` 이 생성되면 빌드 통과. 본 에이전트는 ESP-IDF 미설치 환경이라
> 컴파일은 미실행 — **소스 레벨(보드 프로파일/핀/카메라 컨피그)만 검증함.**

---

## 5. 시리얼 검증 체크리스트 (플래시 후, 기영님)

`idf.py monitor` (115200) 로그를 보며 순서대로 확인:

1. [ ] 부팅 로그에 보드명/`esp32-s3-touch-lcd-3.5b` 또는 AXS15231B 디스플레이 init OK.
2. [ ] PSRAM 인식: `Found 8MB PSRAM` 류 로그 (없으면 N16R8 아님 → 보드/플래시 의심).
3. [ ] Wi-Fi 연결 + 기기 IP 출력.
4. [ ] WebSocket 연결: `ws://<4090>:8003/xiaozhi/v1/` 핸드셰이크 성공 로그.
   - 실패 시: 4090 서버(ALI-19) 기동·포트 8003 개방·방화벽 확인 (infra).
5. [ ] 디스플레이에 표정/대기 UI 표시 (백라이트 GPIO6).
6. [ ] 마이크 입력: 말하면 STT 결과가 서버 로그/화면에 뜸 (ES8311 codec OK).
7. [ ] 스피커 출력: TTS 응답이 들림 (EdgeTTS).
8. [ ] **카메라 비전:** "사진 찍어서 뭐가 보이는지 말해줘" → 카메라 캡처 로그 →
   서버 비전(Qwen3.6) → **한국어 설명 음성**. (= M3 최종 완료 기준)
   - 카메라 init 실패 시: OV5640/OV2640 자동감지 로그 확인, DVP 핀(§2)·리본 케이블 점검.

> 한 항목이라도 막히면 그 단계에서 멈추고 로그를 캡처해 공유. "동작 확인됨"은
> 8번까지 통과했을 때만.

---

## 6. infra-engineer 와 조율할 사항 (ALI-19 / ALI-20)

1. **ALI-20 (커스텀 WebSocket 펌웨어) 재검토 필요.** xiaozhi 공식 펌웨어가
   WebSocket 오디오 + MCP + 카메라 비전을 전부 제공 → 우리가 프로토콜을 새로
   구현할 이유가 사라짐. **ALI-20 은 cancel 또는 scope 축소 후보.**
2. **서버(`config.yaml`) 측 요청:**
   - `VLLM`(비전) 프로바이더 = 우리 Qwen3.6-27B vLLM(OpenAI 호환, 4090:8000).
   - WebSocket 노출 = `:8003/xiaozhi/v1/` (펌웨어 기본 경로와 일치 확인).
   - 페르소나/한국어 락은 서버 프롬프트에서.
3. 메시지: `shared-memory/messages/` 경유로 전달함 (직접 통신 금지 규칙).
