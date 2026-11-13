/*
 * gen_capture_pipelines.c - host-side generator (NOT part of the driver).
 *
 * Builds IPC3 messages for the two capture paths used on volmar:
 *
 *   Pipeline 3: SSP0.IN -> buffer -> volume -> buffer -> PCM1C
 *               (NAU88L25 headset microphone, 48 kHz stereo slots)
 *   Pipeline 4: DMIC0.IN -> buffer -> volume -> buffer -> PCM99C
 *               (internal two-channel PDM microphone array)
 *
 * The layouts come from SOF v2.2.1 IPC headers, matching sof-adl.ri. The
 * DMIC parameters follow sof-adl-nau8825.m4 plus volmar's coreboot
 * mic-tplg="_2ch_pdm0" setting.
 *
 * Build & run:
 *   cc -I<sof-v2.2.1>/src/include -DCONFIG_IPC_MAJOR_3=1 \
 *      gen_capture_pipelines.c -lm -o gen && ./gen > sof_capture_pipelines.h
 */
#define CONFIG_IPC_MAJOR_3 1
#define BIT(x) (1u << (x))
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <math.h>
#include <ipc/header.h>
#include <ipc/topology.h>
#include <ipc/dai.h>
#include <ipc/stream.h>
#include <ipc/control.h>

/* UUIDs stored in the SOF v2.2.1 topology. */
static const uint8_t UUID_HOST[16] = {0x0c,0x10,0x9d,0x8b,0x78,0x6d,0x8f,0x41,0x90,0xa3,0xe0,0xe8,0x05,0xd0,0x85,0x2b};
static const uint8_t UUID_PGA[16]  = {0x7e,0x67,0x7e,0xb7,0xf4,0x5f,0x88,0x41,0xaf,0x14,0xfb,0xa8,0xbd,0xbf,0x86,0x82};
static const uint8_t UUID_DAI[16]  = {0x27,0x0d,0xb0,0xc2,0xbc,0xff,0x50,0x41,0xa5,0x1a,0x24,0x5c,0x79,0xc5,0xe5,0x4b};

typedef struct path_out {
    const char *symbol;
    const char *label;
    int nmsg;
    char names[16][64];
    char descs[16][96];
} path_out;

static uint32_t q816(double db)
{
    return (uint32_t)lround(65536.0 * pow(10.0, db / 20.0));
}

static void emit(const char *name, const void *p, size_t n)
{
    const uint8_t *b = p;
    printf("static const UINT8 %s[%zu] = {", name, n);
    for (size_t i = 0; i < n; i++)
        printf("%s0x%02x", i == 0 ? "\n    " : ((i % 16) ? "," : ",\n    "), b[i]);
    printf("\n};\n");
}

static void add(path_out *o, const char *desc, const void *p, size_t n)
{
    snprintf(o->names[o->nmsg], sizeof(o->names[0]), "c_Sof%sMsg%02d", o->symbol, o->nmsg);
    snprintf(o->descs[o->nmsg], sizeof(o->descs[0]), "%s", desc);
    emit(o->names[o->nmsg], p, n);
    o->nmsg++;
}

static void comp_init(struct sof_ipc_comp *c, uint32_t total, uint32_t id,
                      uint32_t type, uint32_t pipe_id, int has_uuid)
{
    c->hdr.size = total;
    c->hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_COMP_NEW;
    c->id = id;
    c->type = type;
    c->pipeline_id = pipe_id;
    c->core = 0;
    c->ext_data_length = has_uuid ? 16 : 0;
}

static void connect(path_out *o, uint32_t src, uint32_t sink, const char *desc)
{
    struct sof_ipc_pipe_comp_connect c;
    memset(&c, 0, sizeof(c));
    c.hdr.size = sizeof(c);
    c.hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_COMP_CONNECT;
    c.source_id = src;
    c.sink_id = sink;
    add(o, desc, &c, sizeof(c));
}

static void make_ssp0_config(struct sof_ipc_dai_config *d, uint8_t flags)
{
    memset(d, 0, sizeof(*d));
    d->hdr.size = sizeof(*d);
    d->hdr.cmd = SOF_IPC_GLB_DAI_MSG | SOF_IPC_DAI_CONFIG;
    d->type = SOF_DAI_INTEL_SSP;
    d->dai_index = 0;
    d->format = SOF_DAI_FMT_I2S | SOF_DAI_FMT_NB_NF | SOF_DAI_FMT_CBC_CFC;
    d->flags = flags;
    d->ssp.mclk_rate = 24576000;
    d->ssp.bclk_rate = 3072000;
    d->ssp.fsync_rate = 48000;
    d->ssp.tdm_slots = 2;
    d->ssp.tdm_slot_width = 32;
    d->ssp.mclk_direction = 1;
    d->ssp.tx_slots = 3;
    d->ssp.rx_slots = 3;
    d->ssp.sample_valid_bits = 32;
    d->ssp.clks_control = SOF_DAI_INTEL_SSP_CLKCTRL_BCLK_ES;
}

static void make_dmic0_config(struct sof_ipc_dai_config *d, uint8_t flags)
{
    memset(d, 0, sizeof(*d));
    d->hdr.size = sizeof(*d);
    d->hdr.cmd = SOF_IPC_GLB_DAI_MSG | SOF_IPC_DAI_CONFIG;
    d->type = SOF_DAI_INTEL_DMIC;
    d->dai_index = 0;
    /* Linux's IPC3 DMIC loader leaves the generic format field at zero; the
     * DAI type and DMIC-specific parameters fully describe this interface. */
    d->format = 0;
    d->flags = flags;

    d->dmic.driver_ipc_version = 1;
    d->dmic.pdmclk_min = 2400000;
    d->dmic.pdmclk_max = 4800000;
    d->dmic.fifo_fs = 48000;
    d->dmic.fifo_bits = 32;
    d->dmic.duty_min = 40;
    d->dmic.duty_max = 60;
    d->dmic.num_pdm_active = 1;
    d->dmic.unmute_ramp_time = 200;
    d->dmic.pdm[0].id = 0;
    d->dmic.pdm[0].enable_mic_a = 1;
    d->dmic.pdm[0].enable_mic_b = 1;
}

static void make_dai_config(struct sof_ipc_dai_config *d, int dmic, uint8_t flags)
{
    if (dmic) make_dmic0_config(d, flags);
    else make_ssp0_config(d, flags);
}

static void generate_path(path_out *o, uint32_t pipe_id, uint32_t pcm_id,
                          uint32_t id_dai, uint32_t id_host, uint32_t id_buf0,
                          uint32_t id_pga, uint32_t id_buf1, uint32_t id_pipe,
                          int dmic)
{
    char desc[96];

    /* Scheduler: the capture DAI drives the pipeline. */
    {
        struct sof_ipc_pipe_new p;
        memset(&p, 0, sizeof(p));
        p.hdr.size = sizeof(p);
        p.hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_PIPE_NEW;
        p.comp_id = id_pipe;
        p.pipeline_id = pipe_id;
        p.sched_id = id_dai;
        p.core = 0;
        p.period = 1000;
        p.period_mips = 5000;
        p.time_domain = SOF_TIME_DOMAIN_TIMER;
        snprintf(desc, sizeof(desc), "PIPE_NEW pipeline %u", pipe_id);
        add(o, desc, &p, sizeof(p));
    }

    /* Host capture PCM: 16-bit stereo is what the Windows endpoint exposes. */
    {
        uint8_t buf[sizeof(struct sof_ipc_comp_host) + 16];
        struct sof_ipc_comp_host *h = (void *)buf;
        memset(buf, 0, sizeof(buf));
        comp_init(&h->comp, sizeof(buf), id_host, SOF_COMP_HOST, pipe_id, 1);
        h->config.hdr.size = sizeof(h->config);
        h->config.periods_sink = 0;
        h->config.periods_source = 2;
        h->config.frame_fmt = SOF_IPC_FRAME_S16_LE;
        h->direction = SOF_IPC_STREAM_CAPTURE;
        memcpy(buf + sizeof(*h), UUID_HOST, 16);
        snprintf(desc, sizeof(desc), "COMP_NEW host PCM%uC", pcm_id);
        add(o, desc, buf, sizeof(buf));
    }

    /* Host-side buffer. */
    {
        struct sof_ipc_buffer b;
        memset(&b, 0, sizeof(b));
        b.comp.hdr.size = sizeof(b);
        b.comp.hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_BUFFER_NEW;
        b.comp.id = id_buf0;
        b.comp.type = SOF_COMP_BUFFER;
        b.comp.pipeline_id = pipe_id;
        b.size = 768; /* 2 periods * 48 frames * 2 channels * 4 bytes */
        b.caps = 113;
        snprintf(desc, sizeof(desc), "BUFFER_NEW capture %u host", pipe_id);
        add(o, desc, &b, sizeof(b));
    }

    /* Volume also performs the S32 DAI -> S16 host format conversion. */
    {
        uint8_t buf[sizeof(struct sof_ipc_comp_volume) + 16];
        struct sof_ipc_comp_volume *v = (void *)buf;
        memset(buf, 0, sizeof(buf));
        comp_init(&v->comp, sizeof(buf), id_pga, SOF_COMP_VOLUME, pipe_id, 1);
        v->config.hdr.size = sizeof(v->config);
        v->config.periods_sink = 2;
        v->config.periods_source = 2;
        v->config.frame_fmt = SOF_IPC_FRAME_S32_LE;
        v->channels = 2;
        v->min_value = q816(-50.0);
        v->max_value = q816(30.0);
        v->ramp = SOF_VOLUME_LINEAR;
        v->initial_ramp = 250;
        memcpy(buf + sizeof(*v), UUID_PGA, 16);
        snprintf(desc, sizeof(desc), "COMP_NEW volume capture %u", pipe_id);
        add(o, desc, buf, sizeof(buf));

        uint8_t cbuf[sizeof(struct sof_ipc_ctrl_data) + 2 * sizeof(struct sof_ipc_ctrl_value_chan)];
        struct sof_ipc_ctrl_data *c = (void *)cbuf;
        memset(cbuf, 0, sizeof(cbuf));
        c->rhdr.hdr.size = sizeof(cbuf);
        c->rhdr.hdr.cmd = SOF_IPC_GLB_COMP_MSG | SOF_IPC_COMP_SET_VALUE;
        c->comp_id = id_pga;
        c->type = SOF_CTRL_TYPE_VALUE_CHAN_SET;
        c->cmd = SOF_CTRL_CMD_VOLUME;
        c->num_elems = 2;
        c->chanv[0].channel = 0; c->chanv[0].value = q816(0.0);
        c->chanv[1].channel = 1; c->chanv[1].value = q816(0.0);
        snprintf(desc, sizeof(desc), "COMP_SET_VALUE capture %u 0 dB", pipe_id);
        add(o, desc, cbuf, sizeof(cbuf));
        printf("#define SOF_%s_VOL_MSG_INDEX %d\n", o->symbol, o->nmsg - 1);
        printf("#ifndef SOF_CTRL_CHANV0_VALUE_OFF\n#define SOF_CTRL_CHANV0_VALUE_OFF %zu\n#define SOF_CTRL_CHANV_STRIDE %zu\n#endif\n",
               offsetof(struct sof_ipc_ctrl_data, chanv) + offsetof(struct sof_ipc_ctrl_value_chan, value),
               sizeof(struct sof_ipc_ctrl_value_chan));
    }

    /* DAI-side buffer. */
    {
        struct sof_ipc_buffer b;
        memset(&b, 0, sizeof(b));
        b.comp.hdr.size = sizeof(b);
        b.comp.hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_BUFFER_NEW;
        b.comp.id = id_buf1;
        b.comp.type = SOF_COMP_BUFFER;
        b.comp.pipeline_id = pipe_id;
        b.size = 768;
        b.caps = 113;
        snprintf(desc, sizeof(desc), "BUFFER_NEW capture %u DAI", pipe_id);
        add(o, desc, &b, sizeof(b));
    }

    /* Capture DAI and its static configuration. */
    {
        uint8_t buf[sizeof(struct sof_ipc_comp_dai) + 16];
        struct sof_ipc_comp_dai *d = (void *)buf;
        memset(buf, 0, sizeof(buf));
        comp_init(&d->comp, sizeof(buf), id_dai, SOF_COMP_DAI, pipe_id, 1);
        d->config.hdr.size = sizeof(d->config);
        d->config.periods_sink = 2;
        d->config.periods_source = 0;
        d->config.frame_fmt = SOF_IPC_FRAME_S32_LE;
        d->direction = SOF_IPC_STREAM_CAPTURE;
        d->dai_index = 0;
        d->type = dmic ? SOF_DAI_INTEL_DMIC : SOF_DAI_INTEL_SSP;
        memcpy(buf + sizeof(*d), UUID_DAI, 16);
        snprintf(desc, sizeof(desc), "COMP_NEW dai %s.IN", dmic ? "DMIC0" : "SSP0");
        add(o, desc, buf, sizeof(buf));

        struct sof_ipc_dai_config dc;
        make_dai_config(&dc, dmic, SOF_DAI_CONFIG_FLAGS_NONE);
        snprintf(desc, sizeof(desc), "DAI_CONFIG %s", dmic ? "DMIC0" : "SSP0");
        add(o, desc, &dc, sizeof(dc));
    }

    /* Data flows from the DAI toward the host. */
    connect(o, id_dai,  id_buf1, "CONNECT capture DAI -> BUF1");
    connect(o, id_buf1, id_pga,  "CONNECT capture BUF1 -> PGA");
    connect(o, id_pga,  id_buf0, "CONNECT capture PGA -> BUF0");
    connect(o, id_buf0, id_host, "CONNECT capture BUF0 -> HOST");

    {
        struct sof_ipc_pipe_ready r;
        memset(&r, 0, sizeof(r));
        r.hdr.size = sizeof(r);
        r.hdr.cmd = SOF_IPC_GLB_TPLG_MSG | SOF_IPC_TPLG_PIPE_COMPLETE;
        r.comp_id = id_pipe;
        snprintf(desc, sizeof(desc), "PIPE_COMPLETE pipeline %u", pipe_id);
        add(o, desc, &r, sizeof(r));
    }

    printf("\nstatic const SOF_IPC_BLOB c_Sof%sSetup[%d] = {\n", o->symbol, o->nmsg);
    for (int i = 0; i < o->nmsg; i++)
        printf("    { \"%s\", sizeof(%s), %s },\n", o->descs[i], o->names[i], o->names[i]);
    printf("};\n\n");

    /* Stream-time DAI HW_PARAMS/HW_FREE templates. */
    {
        struct sof_ipc_dai_config dc;
        char name[64];
        make_dai_config(&dc, dmic, SOF_DAI_CONFIG_FLAGS_HW_PARAMS);
        snprintf(name, sizeof(name), "c_Sof%sDaiHwParams", o->symbol);
        emit(name, &dc, sizeof(dc));
        make_dai_config(&dc, dmic, SOF_DAI_CONFIG_FLAGS_HW_FREE);
        snprintf(name, sizeof(name), "c_Sof%sDaiHwFree", o->symbol);
        emit(name, &dc, sizeof(dc));
    }

    {
        struct sof_ipc_pcm_params p;
        char name[64];
        memset(&p, 0, sizeof(p));
        p.hdr.size = sizeof(p);
        p.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_PCM_PARAMS;
        p.comp_id = id_host;
        p.params.hdr.size = sizeof(p.params);
        p.params.buffer.hdr.size = sizeof(p.params.buffer);
        p.params.direction = SOF_IPC_STREAM_CAPTURE;
        p.params.frame_fmt = SOF_IPC_FRAME_S16_LE;
        p.params.buffer_fmt = SOF_IPC_BUFFER_INTERLEAVED;
        p.params.rate = 48000;
        p.params.channels = 2;
        p.params.sample_valid_bytes = 2;
        p.params.sample_container_bytes = 2;
        p.params.no_stream_position = 1;
        snprintf(name, sizeof(name), "c_Sof%sPcmParams", o->symbol);
        emit(name, &p, sizeof(p));
        printf("#ifndef SOF_PCM_PARAMS_OFF_BUF_PHY\n");
        printf("#define SOF_PCM_PARAMS_OFF_BUF_PHY %zu\n", offsetof(struct sof_ipc_pcm_params, params.buffer.phy_addr));
        printf("#define SOF_PCM_PARAMS_OFF_BUF_PAGES %zu\n", offsetof(struct sof_ipc_pcm_params, params.buffer.pages));
        printf("#define SOF_PCM_PARAMS_OFF_BUF_SIZE %zu\n", offsetof(struct sof_ipc_pcm_params, params.buffer.size));
        printf("#define SOF_PCM_PARAMS_OFF_STREAM_TAG %zu\n", offsetof(struct sof_ipc_pcm_params, params.stream_tag));
        printf("#define SOF_PCM_PARAMS_OFF_PERIOD_BYTES %zu\n", offsetof(struct sof_ipc_pcm_params, params.host_period_bytes));
        printf("#define SOF_PCM_PARAMS_REPLY_SIZE %zu\n", sizeof(struct sof_ipc_pcm_params_reply));
        printf("#define SOF_PCM_PARAMS_REPLY_OFF_POSN %zu\n#endif\n\n", offsetof(struct sof_ipc_pcm_params_reply, posn_offset));
    }

    {
        struct sof_ipc_stream s;
        char name[64];
        memset(&s, 0, sizeof(s));
        s.hdr.size = sizeof(s);
        s.comp_id = id_host;
        s.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_TRIG_START;
        snprintf(name, sizeof(name), "c_Sof%sTrigStart", o->symbol);
        emit(name, &s, sizeof(s));
        s.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_TRIG_STOP;
        snprintf(name, sizeof(name), "c_Sof%sTrigStop", o->symbol);
        emit(name, &s, sizeof(s));
        s.hdr.cmd = SOF_IPC_GLB_STREAM_MSG | SOF_IPC_STREAM_PCM_FREE;
        snprintf(name, sizeof(name), "c_Sof%sPcmFree", o->symbol);
        emit(name, &s, sizeof(s));
    }
}

int main(void)
{
    path_out micjack = {0};
    path_out dmic = {0};
    micjack.symbol = "MICJACK";
    micjack.label = "headset microphone";
    dmic.symbol = "DMIC";
    dmic.label = "internal microphone array";

    printf("/* GENERATED by tools-gen/gen_capture_pipelines.c using SOF v2.2.1 IPC headers.\n"
           " * Do not edit by hand. */\n#pragma once\n\n");
    printf("#define SOF_MICJACK_PIPE_ID 3\n#define SOF_DMIC_PIPE_ID 4\n\n");

    generate_path(&micjack, 3, 1, 30, 31, 32, 33, 34, 35, 0);
    printf("\n");
    generate_path(&dmic, 4, 99, 40, 41, 42, 43, 44, 45, 1);

    fprintf(stderr, "micjack messages %d, dmic messages %d, dai config %zu, pcm params %zu\n",
            micjack.nmsg, dmic.nmsg, sizeof(struct sof_ipc_dai_config), sizeof(struct sof_ipc_pcm_params));
    return 0;
}
