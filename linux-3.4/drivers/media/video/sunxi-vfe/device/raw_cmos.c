
/*
* A V4L2 driver for GalaxyCore gc2035 cameras.
*
*/
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/videodev2.h>
#include <linux/clk.h>
#include <media/v4l2-device.h>
#include <media/v4l2-chip-ident.h>
#include <media/v4l2-mediabus.h>
#include <linux/io.h>

#include "camera.h"

#define MCLK_DEFAULT 24
static unsigned int mclk = MCLK_DEFAULT;
module_param(mclk, uint, 0);
MODULE_PARM_DESC(mclk, "mclk override (in MHz) (default=24)");

MODULE_AUTHOR ("raymonxiu");
MODULE_AUTHOR ("BobSmith@OLogicinc.com");
MODULE_AUTHOR ("audrey steever");
MODULE_DESCRIPTION ("A CPI camera driver without an I2C interface");
MODULE_LICENSE ("GPL");

#define SENSOR_NAME     "raw_cmos"
#define FILE_NAME       "raw_cmos.c"

//for internel driver debug
#define DEV_DBG_EN           0
#if(DEV_DBG_EN == 1)
#define vfe_dev_dbg(fmt, arg...) printk(KERN_DEBUG "[DEBUG]" SENSOR_NAME ": "fmt"\n", ##arg)

//#define vfe_dev_dbg(fmt, arg...) printk(KERN_DEBUG "[DEBUG]" SENSOR_NAME ": "fmt" (%s:%d)\n", ##arg , FILE_NAME, __LINE__)

#define debug_line               printk(KERN_DEBUG "[DEBUG]" SENSOR_NAME ": "%s: %s : %d\n", FILE_NAME, __func__, __LINE__);
#define debug_profile(x, arg...) printk(KERN_DEBUG "[DEBUG]" SENSOR_NAME ": "x" (%s:%d)\n", ##arg, FILE_NAME, __LINE__);
#define function_profile         printk(KERN_DEBUG "[DEBUG]" SENSOR_NAME ": %s (%s:%d)\n", __func__, FILE_NAME, __LINE__);
#else
#define vfe_dev_dbg(x, arg...)
#define debug_line
#define debug_profile(x, arg...)
#endif

#define vfe_dev_err(x,arg...)   printk(KERN_ERR "[ERR]" SENSOR_NAME ": "x"\n",##arg)
#define vfe_dev_print(x,arg...) printk(KERN_INFO SENSOR_NAME ": "x"\n",##arg)


// module timing
#define VREF_POL            V4L2_MBUS_VSYNC_ACTIVE_HIGH
#define HREF_POL            V4L2_MBUS_HSYNC_ACTIVE_HIGH
#define CLK_POL             V4L2_MBUS_PCLK_SAMPLE_RISING
#define REG_CLKRC           0xfa
#define V4L2_IDENT_SENSOR   0x0001


#define CSI_STBY_ON         1
#define CSI_STBY_OFF        0
#define CSI_RST_ON          0
#define CSI_RST_OFF         1
#define CSI_PWR_ON          1
#define CSI_PWR_OFF         0

#define CSI_GPIO_INPUT      0
#define CSI_GPIO_OUTPUT     1

#define regval_list         reg_list_a8_d8

// NOTE: start at the bottom and read up, it makes a ton more sense that way

// helper: retrieve the sensor_info* pointer from v4l2_subdev*
static inline struct sensor_info* to_state(struct v4l2_subdev* sd) { return container_of(sd, struct sensor_info, sd); }


static struct sensor_format_struct {
    __u8* desc;
    //__u32 pixelformat;
    enum v4l2_mbus_pixelcode mbus_code;
    struct regval_list* regs;
    int regs_size;
    int bpp; // Bytes per pixel
};


// ---------------------------------------------------------------------------- sensor modes

static struct sensor_format_struct sensor_formats[] = { // local
    {
        .desc = "Raw 8-bit CMOS data",
        .mbus_code = V4L2_MBUS_FMT_GREY8_1X8,
        .regs = 0,
        .regs_size = 0,
        .bpp = 1
    },
};
#define N_FMTS ARRAY_SIZE(sensor_formats)

static struct sensor_win_size sensor_win_sizes[] = { // drivers/media/video/sunxi-vfe/device/camera_cfg.h
    {
        .width = 2048,
        .height = 2048,
        .hoffset = 0,
        .voffset = 0,
        .regs = 0,
        .regs_size = 0,
        .set_size = NULL,
    },
};
#define N_WIN_SIZES  (ARRAY_SIZE(sensor_win_sizes))


// ---------------------------------------------------------------------------- handle cases of sensor_ioctl

static int sensor_g_exif(struct v4l2_subdev* sd, struct sensor_exif_attribute* exif)
{
    int ret = 0;
    // meaningless placeholder (we can't get EXIF data, we don't have a CCI) to preserve structure
    gain = val;
    exif->fnumber = 0;
    exif->focal_length = 0;
    exif->brightness = 0;
    exif->flash_fire = 0;
    exif->iso_speed = 0;
    exif->exposure_time_num = 0;
    exif->exposure_time_den = 0;
    return ret;
}


// ---------------------------------------------------------------------------- fields of struct v4l2_subdev_video_ops sensor_video_ops

static int sensor_enum_fmt(struct v4l2_subdev* sd, unsigned index, enum v4l2_mbus_pixelcode* code)
{
    if (index >= N_FMTS) return -EINVAL;

    *code = sensor_formats[index].mbus_code;
    return 0;
}

static int sensor_enum_size(struct v4l2_subdev* sd, struct v4l2_frmsizeenum* fsize)
{
    if (fsize->index > N_WIN_SIZES - 1) return -EINVAL;

    fsize->type = V4L2_FRMSIZE_TYPE_DISCRETE;
    fsize->discrete.width = sensor_win_sizes[fsize->index].width;
    fsize->discrete.height = sensor_win_sizes[fsize->index].height;

    return 0;
}

static int sensor_try_fmt(struct v4l2_subdev* sd, struct v4l2_mbus_framefmt* fmt) // TODO
{
    int index;
    struct sensor_win_size* wsize;

    for (index = 0; index < N_FMTS; index++) if (sensor_formats[index].mbus_code == fmt->code) break;

    if (index >= N_FMTS) return -EINVAL;

    fmt->field = V4L2_FIELD_NONE;

    /*
    * Round requested image size down to the nearest
    * we support, but not below the smallest.
    */
    for (wsize = sensor_win_sizes; wsize < sensor_win_sizes + N_WIN_SIZES; wsize++) {
        if (fmt->width >= wsize->width && fmt->height >= wsize->height) break;
    }

    if (wsize >= sensor_win_sizes + N_WIN_SIZES) wsize--; /* Take the smallest one */

    /*
    * Note the size we'll actually handle.
    */
    fmt->width = wsize->width;
    fmt->height = wsize->height;
    //pix->bytesperline = pix->width*sensor_formats[index].bpp;
    //pix->sizeimage = pix->height*pix->bytesperline;

    return 0;
}

static int sensor_s_fmt(struct v4l2_subdev* sd, struct v4l2_mbus_framefmt* fmt) // TODO
{
    int ret;
    unsigned int temp = 0, shutter = 0;
    unsigned char val;

    struct sensor_format_struct* sensor_fmt;
    struct sensor_win_size* wsize;
    struct sensor_info* info = to_state(sd);

    //    printk("chr wsize.width = [%d], wsize.height = [%d]\n", wsize->width, wsize->height);        
    //vfe_dev_dbg("sensor_s_fmt\n");

    //////////////shutter-gain///////////////
    ret = sensor_try_fmt_internal(sd, fmt, &sensor_fmt, &wsize);
    if (ret) return ret;

    if (hres == 0) {
        if ((wsize->width == VGA_WIDTH) && (wsize->height == VGA_HEIGHT)) {
            nMCLK = (34 * 1000 * 1000);
            nSENSOR_FRAME_RATE = 25;
        }
    }

    sensor_write_array(sd, sensor_fmt->regs, sensor_fmt->regs_size);

    ret = 0;
    if (wsize->regs) {
        ret = sensor_write_array(sd, wsize->regs, wsize->regs_size);
        if (ret < 0) return ret;
    }

    if (wsize->set_size) {
        ret = wsize->set_size(sd);
        if (ret < 0) return ret;
    }


    info->fmt = sensor_fmt;
    info->width = wsize->width;
    info->height = wsize->height;

    return 0;
}

static int sensor_g_parm(struct v4l2_subdev* sd, struct v4l2_streamparm* parms) // TODO
{
    struct v4l2_captureparm* cp = &parms->parm.capture;
    struct sensor_info* info = to_state(sd);

    if (parms->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) return -EINVAL;

    memset(cp, 0, sizeof(struct v4l2_captureparm));
    cp->capability = V4L2_CAP_TIMEPERFRAME;
    cp->timeperframe.numerator = 1;

    cp->timeperframe.denominator = nSENSOR_FRAME_RATE;

    return 0;
}

static int sensor_s_parm(struct v4l2_subdev* sd, struct v4l2_streamparm* parms) // TODO
{
    function_profile;
    struct v4l2_captureparm* cp = &parms->parm.capture;
    struct v4l2_fract* tpf = &cp->timeperframe;
    struct sensor_info* info = to_state(sd);
    int div;
    int clkrc;

    if (parms->type != V4L2_BUF_TYPE_VIDEO_CAPTURE) return -EINVAL;
    if (cp->extendedmode != 0) return -EINVAL;

    if (tpf->numerator == 0 || tpf->denominator == 0) {
        div = 1; /* Reset to full rate */
    } else {
        div = (tpf->numerator * nSENSOR_FRAME_RATE) / tpf->denominator;
    }

    if (div == 0) div = 1;
    else if (div > 8) div = 8;
    tpf->numerator = 1;
    tpf->denominator = nSENSOR_FRAME_RATE / div;
    return 0;
}

static int sensor_g_mbus_config(struct v4l2_subdev* sd, struct v4l2_mbus_config* cfg)
{
    function_profile;
    cfg->type = V4L2_MBUS_PARALLEL;
    cfg->flags = V4L2_MBUS_MASTER | VREF_POL | HREF_POL | CLK_POL;
    return 0;
}


// ---------------------------------------------------------------------------- fields of struct v4l2_subdev_core_ops sensor_core_ops

static int sensor_g_chip_ident(struct v4l2_subdev* sd, struct v4l2_dbg_chip_ident* chip)
{
    function_profile;
    chip->ident = V4L2_IDENT_UNKNOWN;
    chip->revision = 0;
    return 0;
}

static int sensor_g_ctrl(struct v4l2_subdev* sd, struct v4l2_control* ctrl)
{
    function_profile;
    return -EINVAL;
}

static int sensor_s_ctrl(struct v4l2_subdev* sd, struct v4l2_control* ctrl)
{
    function_profile;
    return -EINVAL;
}

static int sensor_queryctrl(struct v4l2_subdev* sd, struct v4l2_queryctrl* qc)
{
    function_profile;
    return -EINVAL;
}

static int sensor_reset(struct v4l2_subdev* sd, u32 val)
{
    function_profile;
    switch (val) {
        case 0:
            vfe_gpio_write(sd, RESET, CSI_RST_OFF);
            usleep_range(10000, 12000);
            break;
        case 1:
            vfe_gpio_write(sd, RESET, CSI_RST_ON);
            usleep_range(10000, 12000);
            break;
        default:
            return -EINVAL;
    }

    return 0;
}

static int sensor_init(struct v4l2_subdev* sd, u32 val)
{
    // identify and boot the sensor
    function_profile;
    return 0;
}

static int sensor_power(struct v4l2_subdev* sd, int on)
{
    int ret;
    function_profile;

    switch (on) {
        case CSI_SUBDEV_STBY_ON:
            debug_profile("CSI_SUBDEV_STBY_ON");

            vfe_gpio_write(sd, PWDN, CSI_STBY_ON);
            usleep_range(10000, 12000);
            vfe_set_mclk(sd, OFF);
            break;
        case CSI_SUBDEV_STBY_OFF:
            debug_profile("CSI_SUBDEV_STBY_OFF");

            vfe_gpio_write(sd, PWDN, CSI_STBY_OFF);
            usleep_range(10000, 12000);

            vfe_set_mclk_freq(sd, mclk);
            vfe_set_mclk(sd, ON);
            usleep_range(10000, 12000);
            break;
        case CSI_SUBDEV_PWR_ON:
            debug_profile("CSI_SUBDEV_PWR_ON");
            vfe_gpio_set_status(sd, PWDN, CSI_GPIO_OUTPUT);
            vfe_gpio_set_status(sd, RESET, CSI_GPIO_OUTPUT);
            vfe_set_mclk_freq(sd, mclk);
            vfe_set_mclk(sd, ON);
            vfe_gpio_write(sd, PWDN, CSI_STBY_OFF);
            vfe_gpio_write(sd, RESET, CSI_RST_ON);
            vfe_gpio_write(sd, POWER_EN, CSI_PWR_ON);
            vfe_set_pmu_channel(sd, IOVDD, ON);
            vfe_set_pmu_channel(sd, AVDD, ON);
            vfe_set_pmu_channel(sd, DVDD, ON);
            vfe_set_pmu_channel(sd, AFVDD, ON);
            usleep_range(10000, 12000);
            //reset after power on
            vfe_gpio_write(sd, RESET, CSI_GPIO_HIGH);
            usleep_range(30000, 31000);
            break;
        case CSI_SUBDEV_PWR_OFF:
            debug_profile("CSI_SUBDEV_PWR_OFF");
            vfe_gpio_write(sd, PWDN, CSI_STBY_OFF);
            vfe_gpio_write(sd, RESET, CSI_RST_OFF);
            usleep_range(5000, 12000);
            vfe_gpio_write(sd, RESET, CSI_RST_ON);
            usleep_range(5000, 12000);
            vfe_gpio_write(sd, PWDN, CSI_STBY_ON);
            usleep_range(5000, 12000);
            vfe_gpio_write(sd, RESET, CSI_RST_OFF);
            vfe_gpio_write(sd, POWER_EN, CSI_PWR_OFF);
            vfe_set_pmu_channel(sd, AFVDD, OFF);
            vfe_set_pmu_channel(sd, DVDD, OFF);
            vfe_set_pmu_channel(sd, AVDD, OFF);
            vfe_set_pmu_channel(sd, IOVDD, OFF);
            usleep_range(10000, 12000);
            vfe_set_mclk(sd, OFF);
            vfe_gpio_set_status(sd, RESET, CSI_GPIO_INPUT);
            vfe_gpio_set_status(sd, PWDN, CSI_GPIO_INPUT);
            break;
        default:
            return -EINVAL;
    }
    return 0;
}

static long sensor_ioctl(struct v4l2_subdev* sd, unsigned int cmd, void* arg)
{
    int ret = 0;
    struct sensor_info* info = to_state(sd);
    vfe_dev_dbg("sensor_ioctl: %d\n", cmd);

    switch (cmd) {
        case GET_SENSOR_EXIF:
            debug_profile("GET_SENSOR_EXIF")
            sensor_g_exif(sd, (struct sensor_exif_attribute *)arg);
            return 0;
        case GET_CURRENT_WIN_CFG:
            debug_profile("GET_CURRENT_WIN_CFG")
            break;
        case SET_FPS:
            debug_profile("SET_FPS")
            break;
        case SET_FLASH_CTRL:
            debug_profile("SET_FLASH_CTRL")
            break;
        case ISP_SET_EXP_GAIN:
            debug_profile("ISP_SET_EXP_GAIN")
            break;
        default:
            break;
    }
    return -EINVAL;
}


// ---------------------------------------------------------------------------- fields of struct v4l2_subdev_ops sensor_ops

static const struct v4l2_subdev_core_ops sensor_core_ops = { // include/media/v4l2-subdev.h
    .g_chip_ident = sensor_g_chip_ident,
    .g_ctrl = sensor_g_ctrl,
    .s_ctrl = sensor_s_ctrl,
    .queryctrl = sensor_queryctrl,
    .reset = sensor_reset,
    .init = sensor_init,
    .s_power = sensor_power,
    .ioctl = sensor_ioctl,
};

static const struct v4l2_subdev_video_ops sensor_video_ops = { // include/media/v4l2-subdev.h
    .enum_mbus_fmt = sensor_enum_fmt,
    .enum_framesizes = sensor_enum_size,
    .try_mbus_fmt = sensor_try_fmt,
    .s_mbus_fmt = sensor_s_fmt,
    .s_parm = sensor_s_parm,
    .g_parm = sensor_g_parm,
    .g_mbus_config = sensor_g_mbus_config,
};


// ---------------------------------------------------------------------------- used via v4l2_subdev_init

static const struct v4l2_subdev_ops sensor_ops = { // include/media/v4l2-subdev.h
    .core = &sensor_core_ops,
    .video = &sensor_video_ops,
};


// ---------------------------------------------------------------------------- fields of struct i2c_driver sensor_driver

static struct v4l2_subdev* sd_ptr;

static int sensor_probe(struct i2c_client* client, const struct i2c_device_id* id)
{
    struct sensor_info* info; // in camera.h
    function_profile;

    info = kzalloc(sizeof(struct sensor_info), GFP_KERNEL);
    if (info == nullptr) return -ENOMEM;
    sd_ptr = &info->sd;
    v4l2_subdev_init(sd_ptr, &sensor_ops);

    info->fmt = &sensor_formats[0];

    info->brightness = 0;
    info->contrast = 0;
    info->saturation = 0;
    info->hue = 0;
    info->hflip = 0;
    info->vflip = 0;
    info->gain = 0;
    info->autogain = 0;
    info->exp = 0;
    info->autoexp = 0;
    info->autowb = 0;
    info->wb = 0;
    info->clrfx = 0;

    return 0;
}

static int sensor_remove(struct i2c_client* client)
{
    function_profile;

    v4l2_device_unregister_subdev(sd_ptr);
    kfree(to_state(sd_ptr));
    return 0;
}

static const struct i2c_device_id sensor_id[] = {
    { SENSOR_NAME, 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, sensor_id);


// ---------------------------------------------------------------------------- used in init and exit

static struct i2c_driver sensor_driver = {
    .driver = {
    .owner = THIS_MODULE,
    .name = SENSOR_NAME,
    },
.probe = sensor_probe,
.remove = sensor_remove,
.id_table = sensor_id,
};


// ---------------------------------------------------------------------------- entry/exit points

static __init

int init_sensor(void)
{
    // validate module parameters here
    if (!mclk || mclk >= 35) mclk = MCLK_DEFAULT;

    return sensor_driver.probe(NULL, NULL);
}

static __exit

void exit_sensor(void)
{
    sensor_driver.remove(NULL);
}

module_init (init_sensor);
module_exit (exit_sensor);
