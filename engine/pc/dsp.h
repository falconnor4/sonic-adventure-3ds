/* the AICA's DSP: a program of 128 steps, run once a sample on what the
 * channels send. the mix adds its outputs. its registers are the AICA's own:
 * 128 coefficients, 64 memory addresses and 128 steps of four 16-bit words. an
 * fx bank, SFPB, holds each program as an image of them. the ring, a delay
 * line, holds 16-bit words */
#ifndef PC_DSP_H
#define PC_DSP_H
#include <stdint.h>

namespace pc {

class Dsp {
public:
    /* the ring's least words, 8K, and its most, 64K */
    static constexpr uint32_t RING_LEAST = 1U << 13;
    static constexpr uint32_t RING = 1U << 16;
    /* the bytes of an SFPB program's image of the registers */
    static constexpr uint32_t IMAGE = 0xc00;

    /* load the registers from an image of IMAGE bytes. the coefficients start
     * at 0, the addresses at 0x200 and the steps at 0x400. each register is
     * the low half of a little-endian 32-bit word. the ring holds words,
     * rounded up to a power of two from RING_LEAST, and at most RING. the
     * temporaries, memory and ring start silent, and the DSP runs */
    void load(const uint8_t *image, uint32_t words);
    void stop() { running_ = false; }
    bool running() const { return running_; }
    uint32_t used() const { return used_; }
    /* step's coefficient as its AICA register holds it, 13 bits from bit 3 */
    void set_coefficient(uint32_t step, int16_t value);
    int16_t coefficient(uint32_t step) const { return coef_[step & 127]; }
    /* add a channel's sample, in 16-bit units, to the program's input */
    void send(uint32_t input, int32_t sample);
    /* run the program over what was sent since the last run. it writes its 16
     * outputs in 16-bit units, or 0s where it isn't running */
    void run(int32_t outputs[16]);

private:
    struct Step {
        uint8_t tra, twt, twa, xsel, ysel, ira, iwt, iwa;
        uint8_t table, mwt, mrd, ewt, ewa, adrl, frcl, shift, yrl, negb;
        uint8_t zero, bsel, nofl, masa, adreb, nxadr;
    };

    Step steps_[128] = {};
    /* the steps run, in order, and their count: those that write what lasts,
     * a temporary, the memory, the ring or an output, and those whose
     * accumulator, FRC, Y, ADRS or read a later run step takes (load). the
     * rest compute what nothing reads, as a program's empty steps do, and
     * leaving them out changes nothing */
    uint8_t order_[128] = {};
    /* each step's flags, as one word, and its Y where it takes a
     * coefficient, sign-extended, decoded at load for run */
    enum : uint32_t {
        IN_MEMS = 1, IN_MIXS = 2, IWT = 4, XSEL = 8, YRL = 16, TABLE = 32, MWT = 64,
        MRD = 128, EWT = 256, ADRL = 512, FRCL = 1024, NEGB = 2048, ZERO = 4096,
        BSEL = 8192, NOFL = 16384, ADREB = 32768
    };
    static constexpr uint32_t TWT = 1U << 16;
    uint32_t flags_[128] = {};
    int32_t ycoef_[128] = {};
    uint32_t used_ = 0;
    int16_t coef_[128] = {};
    uint16_t madrs_[64] = {};
    int32_t temp_[128] = {};
    int32_t mems_[32] = {};
    int32_t mixs_[16] = {};
    uint16_t ring_[RING] = {};
    uint32_t ring_mask_ = RING - 1;
    uint32_t dec_ = 0;
    bool running_ = false;
};

} // namespace pc
#endif
