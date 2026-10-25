// daisy_synth: a poly synth on the Electrosmith Seed3 Desktop Dev Kit that the airmidi box finds over MIDI-CI.
//
// The box's TRS MIDI OUT goes to the kit's DIN MIDI IN, the kit's DIN MIDI OUT back to the box's TRS MIDI IN (TRS
// type A adapters on the box's side). The box asks the kit who it is (Discovery) and reads its control list (Property
// Exchange, answered by lib/midici_responder), and its web page draws those controls. Turning a pot or flipping a
// toggle on the kit sends the control's CC back, so the page follows; the buttons play notes and send them too.
//
// This file is the board glue; the synth and the MIDI device are in ../engine (platform-free, checked on the desktop
// by ../host). Pin map (Seed3 Desktop Dev Kit Rev4 schematic, docs in the design workspace):
//   MIDI     USART1: D13 TX -> DIN OUT, D14 RX <- H11L1 <- DIN IN (THRU is hardware)
//   buttons  CD4021 x2: D8 clock, D9 latch, D10 data; S1 shifts out first; pressed = low
//            S1-S8 the back row, S9-S16 the front row (nearer the player)
//   LEDs     PCA9685 @ 0x40 on I2C1 (D11 SCL, D12 SDA), channel i = LED above S(i+1), active high
//   pots     CD4051 into D15, select D24 (A) D25 (B) D26 (C); VR(i+1) = mux channel i
//   toggles  SW17 D19, SW18 D28 (A) / D27 (B), all switch to GND
#include "daisy_seed.h"
#include "dev/leddriver.h"
#include "dev/sr_4021.h"
#include "../engine/device.h"
#include "../engine/synth.h"

using namespace daisy;

namespace
{
// Front row C3..C4, back row C4..C5: one octave of C major per row.
constexpr uint8_t kButtonNote[16] = {60, 62, 64, 65, 67, 69, 71, 72,  // S1-S8 (back)
                                     48, 50, 52, 53, 55, 57, 59, 60}; // S9-S16 (front)
constexpr int     kDebounceScans  = 3;
constexpr float   kPotTakeover    = 0.025f; // a pot takes its parameter back after moving this far
constexpr float   kPotStep        = 1.f / 254.f;

DaisySeed                            hw;
syn::Synth                           synth;
syn::Device                          device;
CpuLoadMeter                         cpu;
UartHandler                          uart;
UartHandler::Config                  uartConfig; // kept: KeepListening sets the UART up again with it
ShiftRegister4021<2>                 buttons;
LedDriverPca9685<1, true>            leds;
LedDriverPca9685<1, true>::DmaBuffer DMA_BUFFER_MEM_SECTION ledBufA, ledBufB;
I2CHandle                            i2c;
GPIO                                 sw17, sw18a, sw18b;

float DSY_SDRAM_BSS echoMem[syn::Synth::kEchoFloats];
float DSY_SDRAM_BSS roomMem[syn::Synth::kRoomFloats];

// Steady load. The audio callback runs every 1 ms (48 samples at 48 kHz) and is FPU and SDRAM work; the rest of the
// millisecond the main loop only polls. The board's supply current then swings at 1 kHz, and that swing is heard
// on the line out as a 1 kHz whine. So while the main loop has nothing to do, it renders a muted second synth (eight
// voices held, its own reverb and echo in SDRAM): the same code and memory traffic as the audio, all the time, so
// the current stays level. Its output is thrown away. steadyLoad = 0 over SWD turns it off for an A/B.
syn::Synth        ballast;
float DSY_SDRAM_BSS ballastEcho[syn::Synth::kEchoFloats];
float DSY_SDRAM_BSS ballastRoom[syn::Synth::kRoomFloats];
volatile uint32_t steadyLoad = 1;

// MIDI IN: the UART's DMA listens into rxDma; its callback (an interrupt) copies into rxRing, the main loop drains it.
uint8_t DMA_BUFFER_MEM_SECTION rxDma[256];
constexpr uint32_t             kRxRing = 1024;
uint8_t                        rxRing[kRxRing];
volatile uint32_t              rxHead = 0;
uint32_t                       rxTail = 0;

// MIDI OUT: bytes wait in txRing and go into the UART's data register from the main loop whenever it has room.
// (libDaisy's UART DMA serves one transfer at a time, and listening holds it, so transmit is by hand.)
constexpr uint32_t kTxRing = 4096;
uint8_t            txRing[kTxRing];
uint32_t           txHead = 0, txTail = 0;

// Read over SWD (`openocd ... -c "mdw <addr>"`), see README.md; dbgNote is the test hook for playing a note
// without a MIDI cable: write 0x100 | note for on, 0x200 | note for off.
struct Debug
{
    float    cpuAvg, cpuMax;
    float    pots[8];
    uint32_t buttons, sw17, sw18, rxBytes, txBytes, ciReplies, muid, voices;
    uint32_t rxRestarts; // MIDI IN listening restarted after a line error (see KeepListening)
} volatile dbg;
volatile uint32_t dbgNote = 0;

void RxCallback(uint8_t* data, size_t size, void*, UartHandler::Result res)
{
    if(res != UartHandler::Result::OK)
        return;
    for(size_t i = 0; i < size; i++)
        rxRing[(rxHead + i) % kRxRing] = data[i];
    rxHead = rxHead + size;
}

// MIDI IN can stop for good after a UART error: on an overrun the HAL clears the DMA request and aborts the DMA
// stream, and the error callback (which would tell libDaisy) only runs once that abort completes, which it never
// did here. libDaisy then still says it is listening while nothing arrives. A glitch on the line causes it (the
// box resetting, a cable plugged in while running): found 2026-10-24 with overrun + noise flags set, DMA receive
// off, 7540 bytes in and then nothing, audio still running. So the main loop looks at the UART itself, and when its
// DMA request is off it stops and starts listening again. A byte or two of the glitch is lost, nothing after it.
// Stopping and starting the listener is not enough: HAL_UART_DMAStop resets the HAL's receive state only while the
// DMA request bit is still set, and the error has already cleared it, so every new start was refused as busy (seen
// after a framing error). Setting the UART up again resets all of the HAL's state. Tries are 2 ms apart; only a
// start that took is counted.
void KeepListening()
{
    static uint32_t lastTry = 0;
    if(uart.IsListening() && (USART1->CR3 & USART_CR3_DMAR))
        return;
    const uint32_t us = System::GetUs();
    if(us - lastTry < 2000)
        return;
    lastTry = us;
    uart.DmaListenStop();
    USART1->ICR = USART_ICR_ORECF | USART_ICR_NECF | USART_ICR_FECF | USART_ICR_PECF;
    uart.Init(uartConfig);
    if(uart.DmaListenStart(rxDma, sizeof(rxDma), RxCallback, nullptr) == UartHandler::Result::OK)
        dbg.rxRestarts = dbg.rxRestarts + 1;
}

void PumpTx()
{
    while(txTail != txHead && (USART1->ISR & USART_ISR_TXE_TXFNF))
    {
        USART1->TDR = txRing[txTail++ % kTxRing];
        dbg.txBytes = dbg.txBytes + 1;
    }
}

void MidiOut(void*, const uint8_t* b, size_t n)
{
    for(size_t i = 0; i < n; i++)
    {
        while(txHead - txTail >= kTxRing) // full (a long MIDI-CI reply): wait for the wire
            PumpTx();
        txRing[txHead++ % kTxRing] = b[i];
    }
}

#ifdef WHINE_TEST
// Test build (make WHINE_TEST=1): which part of the panel scan puts the 1 kHz comb on the line out. Steady load on
// throughout; 5 s per segment, each marked by a 60 ms beep at 750 + 500 * segment Hz, so one recording compares them.
//   0 every scan as shipped  1 no button scan  2 no LED transfers  3 scans at random 0.5-1.5 ms intervals
//   4 random intervals + LEDs sent only when they change  5 no scans
constexpr int     kSegments = 6;
volatile float    beepHz    = 0.f;
volatile uint32_t beepLeft  = 0;
float             beepPh    = 0.f;
#endif
// The panel scan runs every 1000 us from the microsecond timer, not on the 1 ms tick: scanning on the first loop pass
// after each tick kept it at one point of the audio block and measured ~10 dB more 3 kHz on the line out. LEDs are
// sent only when they change, so an idle panel makes no I2C traffic. (The flags are for the WHINE_TEST build.)
bool scanButtons = true, ledsOnChange = true, jitter = false, scans = true;

void AudioCallback(AudioHandle::InputBuffer, AudioHandle::OutputBuffer out, size_t size)
{
    cpu.OnBlockStart();
    synth.process(out[0], out[1], size);
    cpu.OnBlockEnd();
#ifdef WHINE_TEST
    for(size_t i = 0; i < size && beepLeft; i++, beepLeft = beepLeft - 1)
    {
        beepPh += beepHz / 48000.f;
        beepPh -= (int)beepPh;
        const float b = 0.06f * (beepPh < 0.5f ? 4.f * beepPh - 1.f : 3.f - 4.f * beepPh);
        out[0][i] += b;
        out[1][i] += b;
    }
#endif
}

void InitHardware()
{
    UartHandler::Config& uc = uartConfig;
    uc.periph               = UartHandler::Config::Peripheral::USART_1;
    uc.mode                 = UartHandler::Config::Mode::TX_RX;
    uc.baudrate             = 31250;
    uc.pin_config.tx        = seed::D13;
    uc.pin_config.rx        = seed::D14;
    uart.Init(uc);
    uart.DmaListenStart(rxDma, sizeof(rxDma), RxCallback, nullptr);

    ShiftRegister4021<2>::Config src;
    src.clk         = seed::D8;
    src.latch       = seed::D9;
    src.data[0]     = seed::D10;
    src.delay_ticks = System::GetTickFreq() / 1000000; // ~1 us edges for the 4021 at 3.3 V
    buttons.Init(src);

    I2CHandle::Config ic;
    ic.periph         = I2CHandle::Config::Peripheral::I2C_1;
    ic.speed          = I2CHandle::Config::Speed::I2C_400KHZ;
    ic.mode           = I2CHandle::Config::Mode::I2C_MASTER;
    ic.pin_config.scl = seed::D11;
    ic.pin_config.sda = seed::D12;
    i2c.Init(ic);
    const uint8_t addr[1] = {0x00};
    leds.Init(i2c, addr, ledBufA, ledBufB);
    // libDaisy sets MODE2 for LEDs on the outputs' low side (inverted). The kit sources its LEDs from the outputs
    // (output -> 2k -> anode), so: totem pole, not inverted.
    uint8_t mode2[2] = {0x01, 0x04};
    i2c.TransmitBlocking(0x40, mode2, 2, 10);

    sw17.Init(seed::D19, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    sw18a.Init(seed::D28, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);
    sw18b.Init(seed::D27, GPIO::Mode::INPUT, GPIO::Pull::PULLUP);

    AdcChannelConfig ac;
    ac.InitMux(seed::D15, 8, seed::D24, seed::D25, seed::D26);
    hw.adc.Init(&ac, 1);
    hw.adc.Start();
}

// Debounced button scan: a change is accepted after kDebounceScans equal reads (1 ms apart).
uint16_t ScanButtons()
{
    static uint16_t stable = 0, last = 0;
    static int      count  = 0;
    buttons.Update();
    uint16_t raw = 0;
    for(int k = 0; k < 16; k++)
        if(!buttons.State(15 - k)) // S(k+1) is shifted out k-th; pressed pulls low
            raw |= (uint16_t)(1u << k);
    if(raw == last)
    {
        if(count < kDebounceScans)
            count++;
        else
            stable = raw;
    }
    else
    {
        last  = raw;
        count = 0;
    }
    return stable;
}

// A pot edits its parameter only once it has moved: at power-up, and after the parameter came in over MIDI, the
// parameter keeps its value until the pot is turned (no jump on boot, no fight with the page).
struct Pot
{
    float smooth = 0.f, anchor = 0.f, held = 0.f;
    bool  active = false;
};
Pot pots[syn::kPots];

void ScanPots(uint32_t remote)
{
    for(int i = 0; i < syn::kPots; i++)
    {
        Pot& p = pots[i];
        p.smooth += 0.15f * (hw.adc.GetMuxFloat(0, i) - p.smooth); // ~6 ms smoothing
        dbg.pots[i] = p.smooth;
        if(remote & (1u << i))
        {
            p.active = false;
            p.anchor = p.smooth;
        }
        if(!p.active && (p.smooth - p.anchor > kPotTakeover || p.anchor - p.smooth > kPotTakeover))
        {
            p.active = true;
            p.held   = -1.f;
        }
        if(p.active && (p.smooth - p.held > kPotStep || p.held - p.smooth > kPotStep))
        {
            p.held = p.smooth;
            device.localParam(i, p.held);
        }
    }
}

// Toggles act when they move (debounced), not by where they are at power-up.
// SW18: A -> saw, middle -> square, B -> triangle. SW17: glide on when D19 is pulled low.
void ScanToggles()
{
    static int  last17 = -1, last18 = -1, cand17 = -1, cand18 = -1, n17 = 0, n18 = 0;
    const int   s17    = sw17.Read() ? 0 : 1;
    const int   s18    = !sw18a.Read() ? 0 : (!sw18b.Read() ? 2 : 1);
    dbg.sw17           = s17;
    dbg.sw18           = s18;
    auto debounce = [](int now, int& cand, int& n, int& last) {
        if(now != cand)
        {
            cand = now;
            n    = 0;
            return false;
        }
        if(n < kDebounceScans)
        {
            if(++n == kDebounceScans && now != last)
            {
                bool first = last < 0;
                last       = now;
                return !first;
            }
        }
        return false;
    };
    if(debounce(s17, cand17, n17, last17))
        device.localParam(syn::kGlide, (float)s17);
    if(debounce(s18, cand18, n18, last18))
        device.localParam(syn::kWave, (s18 + 0.5f) / syn::kWaves);
}

void ScanKeys(uint32_t now)
{
    static uint16_t prev = 0;
    const uint16_t  b    = scanButtons ? ScanButtons() : prev;
    dbg.buttons          = b;
    for(int k = 0; k < 16; k++)
    {
        const uint16_t m = (uint16_t)(1u << k);
        if((b ^ prev) & m)
            device.localNote(kButtonNote[k], b & m);
    }
    prev = b;
    (void)now;
}

// LED k glows with the envelope of the voice playing S(k+1)'s note; a note outside the buttons' range lights the
// button with the same pitch class. For the first second after power-up, a sweep says the synth is running.
void UpdateLeds(uint32_t now)
{
    float lv[16] = {};
    if(now < 1200)
    {
        const int at = (int)(now / 60) % 16;
        lv[at < 8 ? at + 8 : at - 8] = 1.f; // front row first
    }
    for(int v = 0; v < syn::Synth::kVoices; v++)
    {
        const int note = synth.voiceNote(v);
        if(note < 0)
            continue;
        int k = -1;
        for(int i = 0; i < 16 && k < 0; i++)
            if(kButtonNote[i] == note)
                k = i;
        for(int i = 0; i < 16 && k < 0; i++)
            if(kButtonNote[i] % 12 == note % 12)
                k = i;
        if(k < 0)
            continue;
        const float e = synth.voiceLevel(v);
        if(e > lv[k])
            lv[k] = e;
    }
    static uint8_t sent[16];
    bool           changed = !ledsOnChange;
    for(int i = 0; i < 16; i++)
    {
        const float   v = lv[i] * lv[i]; // squared: closer to how bright it looks
        const uint8_t q = (uint8_t)(v * 255.f);
        changed |= q != sent[i];
        sent[i] = q;
        leds.SetLed(i, v);
    }
    if(changed)
        leds.SwapBuffersAndTransmit();
}

uint32_t rng = 0x9E3779B9u;
uint32_t NextScanGap()
{
    if(!jitter)
        return 1000;
    rng ^= rng << 13, rng ^= rng >> 17, rng ^= rng << 5;
    return 500 + rng % 1001; // us
}
} // namespace

int main()
{
    hw.Init(true); // 480 MHz
    hw.SetAudioBlockSize(48);
    hw.SetAudioSampleRate(SaiHandle::Config::SampleRate::SAI_48KHZ);
    const float sr = hw.AudioSampleRate();

    synth.init(sr, echoMem, roomMem);
    ballast.init(sr, ballastEcho, ballastRoom);
    ballast.setParam(syn::kSustain, 1.f); // the held voices never decay away
    ballast.setParam(syn::kEcho, 0.5f);
    for(int v = 0; v < syn::Synth::kVoices; v++)
        ballast.noteOn((uint8_t)(48 + 5 * v), 100);
    device.init(&synth, Random::GetValue() ^ System::GetUs(), MidiOut, nullptr);
    dbg.muid = device.muid();
    cpu.Init(sr, 48);
    InitHardware();
    System::Delay(30); // the ADC's first pass over the mux: until then every pot reads 0
    for(int i = 0; i < syn::kPots; i++)
        pots[i].smooth = pots[i].anchor = hw.adc.GetMuxFloat(0, i);
    hw.StartAudio(AudioCallback);

    uint32_t nextScan = System::GetUs(), lastLed = 0;
#ifdef WHINE_TEST
    const uint32_t testStart = System::GetNow() + 2000;
    int            applied   = -1;
#endif
    for(;;)
    {
        const uint32_t now = System::GetNow();
#ifdef WHINE_TEST
        const int seg = now < testStart ? -1 : (int)((now - testStart) / 5000) % kSegments;
        if(seg != applied)
        {
            applied = seg;
            if(seg >= 0)
            {
                beepHz   = 750.f + 500.f * seg;
                beepLeft = 2880;
            }
            scanButtons  = seg != 1;
            ledsOnChange = seg == 2 || seg == 4; // seg 2: idle LEDs never change, so nothing is sent
            jitter       = seg == 3 || seg == 4;
            scans        = seg != 5;
        }
#endif

        KeepListening();
        while(rxTail != rxHead)
        {
            device.receive(rxRing[rxTail++ % kRxRing]);
            dbg.rxBytes = dbg.rxBytes + 1;
        }
        dbg.ciReplies = device.ciReplies();

        const uint32_t us = System::GetUs();
        if((int32_t)(us - nextScan) >= 0 && scans)
        {
            nextScan = us + NextScanGap();
            ScanPots(device.takeRemote());
            ScanToggles();
            ScanKeys(now);
            dbg.cpuAvg = cpu.GetAvgCpuLoad();
            dbg.cpuMax = cpu.GetMaxCpuLoad();
            uint32_t active = 0;
            for(int v = 0; v < syn::Synth::kVoices; v++)
                active += synth.voiceNote(v) >= 0;
            dbg.voices = active;
        }

        if(const uint32_t d = dbgNote)
        {
            dbgNote = 0;
            device.localNote(d & 0x7F, d & 0x100);
        }

        if(now - lastLed >= 16 && scans && !(jitter && (rng & 1)))
        {
            lastLed = now;
            UpdateLeds(now);
        }
        PumpTx();

        if(steadyLoad)
        {
            float scratch[2][8]; // ~55 us of work: the loop still comes round far faster than a MIDI byte (320 us)
            ballast.process(scratch[0], scratch[1], 8);
        }
    }
}
