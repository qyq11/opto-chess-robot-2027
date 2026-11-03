#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_clk.h>
#include <linux/pinctrl/consumer.h>

/* 临时诊断：只读现有驱动使用的检查寄存器，不写相机配置。 */
static int bus = 6;
module_param(bus, int, 0444);
static int matches;
module_param(matches, int, 0444);
static const u16 candidates[] = { 0x10, 0x1a, 0x36, 0x37 };

static int __init camera_addr_probe_init(void)
{
	struct device_node *node = NULL, *alternate_node = NULL;
	struct i2c_client *client = NULL, *alternate = NULL;
	struct gpio_desc *reset = NULL;
	struct clk *clock = NULL;
	struct pinctrl *pins = NULL;
	struct pinctrl_state *default_pins;
	const char *controller;
	char node_path[96], alternate_path[96];
	int ret = 0, old_direction = -1, old_raw = -1, restore_ret = 0;
	bool client_locked = false, alternate_locked = false;
	bool clock_enabled = false, reset_changed = false;
	unsigned int i;

	if (bus == 4)
		controller = "2ac70000";
	else if (bus == 5)
		controller = "2ac80000";
	else if (bus == 6)
		controller = "2ac90000";
	else
		return -EINVAL;

	snprintf(node_path, sizeof(node_path), "/i2c@%s/imx415@37", controller);
	snprintf(alternate_path, sizeof(alternate_path), "/i2c@%s/gc05a2@37", controller);
	node = of_find_node_by_path(node_path);
	if (!node) {
		ret = -ENODEV;
		goto cleanup;
	}
	client = of_find_i2c_device_by_node(node);
	if (!client) {
		ret = -ENODEV;
		goto cleanup;
	}
	device_lock(&client->dev);
	client_locked = true;
	if (client->dev.driver || client->adapter->nr != bus) {
		ret = -EBUSY;
		goto cleanup;
	}
	alternate_node = of_find_node_by_path(alternate_path);
	if (alternate_node)
		alternate = of_find_i2c_device_by_node(alternate_node);
	if (alternate) {
		device_lock(&alternate->dev);
		alternate_locked = true;
		if (alternate->dev.driver) {
			ret = -EBUSY;
			goto cleanup;
		}
	}

	clock = of_clk_get(node, 0);
	if (IS_ERR(clock)) {
		ret = PTR_ERR(clock);
		clock = NULL;
		goto cleanup;
	}
	if (clk_get_rate(clock) != 37125000) {
		ret = -ERANGE;
		goto cleanup;
	}

	/* 选择当前设备树已有的时钟引脚配置，不生成新映射。 */
	pins = pinctrl_get(&client->dev);
	if (IS_ERR(pins)) {
		ret = PTR_ERR(pins);
		pins = NULL;
		goto cleanup;
	}
	default_pins = pinctrl_lookup_state(pins, "default");
	if (IS_ERR(default_pins)) {
		ret = PTR_ERR(default_pins);
		goto cleanup;
	}
	ret = pinctrl_select_state(pins, default_pins);
	if (ret)
		goto cleanup;

	reset = gpiod_get_from_of_node(node, "reset-gpios", 0, GPIOD_ASIS,
				     "camera-address-diagnostic");
	if (IS_ERR(reset)) {
		ret = PTR_ERR(reset);
		reset = NULL;
		goto cleanup;
	}
	if (!gpiod_is_active_low(reset)) {
		ret = -EINVAL;
		goto cleanup;
	}
	old_direction = gpiod_get_direction(reset);
	old_raw = gpiod_get_raw_value_cansleep(reset);
	if (old_direction < 0 || old_raw < 0) {
		ret = -EIO;
		goto cleanup;
	}
	pr_info("camaddr: BEGIN bus=%d reset_direction=%d reset_raw=%d clock_hz=%lu\n",
		bus, old_direction, old_raw, clk_get_rate(clock));

	/* 现有供电已常开：先保持复位，再释放、等待、开启时钟。 */
	ret = gpiod_direction_output_raw(reset, 0);
	if (ret)
		goto cleanup;
	reset_changed = true;
	usleep_range(1000, 2000);
	gpiod_set_raw_value_cansleep(reset, 1);
	usleep_range(10, 20);
	ret = clk_prepare_enable(clock);
	if (ret)
		goto cleanup;
	clock_enabled = true;
	usleep_range(20000, 22000);
	pr_info("camaddr: READY bus=%d reset_raw=%d clock_hz=%lu\n",
		bus, gpiod_get_raw_value_cansleep(reset), clk_get_rate(clock));

	for (i = 0; i < ARRAY_SIZE(candidates); i++) {
		u8 register_address[2] = { 0x31, 0x1a };
		u8 value = 0;
		struct i2c_msg messages[2] = {
			{ .addr = candidates[i], .len = 2, .buf = register_address },
			{ .addr = candidates[i], .flags = I2C_M_RD, .len = 1, .buf = &value },
		};
		int transfer_ret = i2c_transfer(client->adapter, messages, 2);
		if (transfer_ret == 2) {
			pr_info("camaddr: READ bus=%d address=0x%02x register=0x311a value=0x%02x expected=0xe0 match=%d\n",
				bus, candidates[i], value, value == 0xe0);
			if (value == 0xe0)
				matches++;
		} else {
			pr_info("camaddr: READ bus=%d address=0x%02x register=0x311a transfer_ret=%d value=UNAVAILABLE\n",
				bus, candidates[i], transfer_ret);
		}
	}
	ret = 0;

cleanup:
	if (reset_changed) {
		gpiod_set_raw_value_cansleep(reset, 0);
		usleep_range(1000, 2000);
	}
	if (clock_enabled)
		clk_disable_unprepare(clock);
	if (reset_changed) {
		if (old_direction == 1)
			restore_ret = gpiod_direction_input(reset);
		else
			restore_ret = gpiod_direction_output_raw(reset, old_raw);
		pr_info("camaddr: RESTORE bus=%d ret=%d direction=%d raw=%d before_direction=%d before_raw=%d\n",
			bus, restore_ret, gpiod_get_direction(reset),
			gpiod_get_raw_value_cansleep(reset), old_direction, old_raw);
	}
	if (reset)
		gpiod_put(reset);
	if (pins)
		pinctrl_put(pins);
	if (clock)
		clk_put(clock);
	if (alternate_locked)
		device_unlock(&alternate->dev);
	if (alternate)
		put_device(&alternate->dev);
	if (client_locked)
		device_unlock(&client->dev);
	if (client)
		put_device(&client->dev);
	of_node_put(alternate_node);
	of_node_put(node);
	pr_info("camaddr: END bus=%d setup_ret=%d restore_ret=%d matches=%d\n",
		bus, ret, restore_ret, matches);
	return ret ? ret : restore_ret;
}

static void __exit camera_addr_probe_exit(void)
{
	pr_info("camaddr: UNLOADED bus=%d\n", bus);
}

module_init(camera_addr_probe_init);
module_exit(camera_addr_probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Temporary bounded camera address diagnostic; no sensor configuration writes");
