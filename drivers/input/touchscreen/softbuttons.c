/*
 * touchscreen softbutton helper functions
 *
 * Copyright (c) 2016 Hans de Goede <hdegoede@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/input.h>
#include <linux/input/touchscreen.h>
#include <linux/leds.h>
#include <linux/of.h>

struct touchscreen_softbutton {
	u32 min_x;
	u32 max_x;
	u32 min_y;
	u32 max_y;
	u32 keycode;
	const char *ledtrigger_name;
	struct led_trigger *ledtrigger;
};

struct touchscreen_softbutton_info {
	struct input_dev *input;
	struct touchscreen_softbutton *buttons;
	int button_count;
};

/**
 * devm_touchscreen_alloc_softbuttons - allocate softbuttons
 * @input: touchscreen input device for which softbuttons should be allocated
 *
 * This function parses touschcreen softbutton DT properties for touchscreens
 * and allocates and fill a touchscreen_softbutton_info struct if any
 * softbuttons are found.
 *
 * Returns prepared struct touchscreen_softbutton_info on success,
 * %NULL if no softbuttons were found (this is not an error) or a ERR_PTR
 * in case of an error.
 *
 * Note as this is a devm function the returned pointer does not need to
 * be freed.
 */
struct touchscreen_softbutton_info *devm_touchscreen_alloc_softbuttons(
					struct input_dev *input)
{
	struct device *dev = input->dev.parent;
	struct device_node *np, *pp;
	struct touchscreen_softbutton_info *info;
	int i, j, err, button_count;

	np = dev->of_node;
	if (!np)
		return NULL;

	button_count = of_get_child_count(np);
	if (button_count == 0)
		return NULL;

	info = devm_kzalloc(dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return ERR_PTR(-ENOMEM);

	info->input = input;
	info->button_count = button_count;
	info->buttons = devm_kzalloc(dev, button_count * sizeof(*info->buttons),
				     GFP_KERNEL);
	if (!info->buttons)
		return ERR_PTR(-ENOMEM);

	for (pp = of_get_next_child(np, NULL), i = 0;
	     pp != NULL;
	     pp = of_get_next_child(np, pp), i++) {
		struct touchscreen_softbutton *btn = &info->buttons[i];

		err = of_property_read_u32(pp, "linux,code", &btn->keycode);
		if (err) {
			dev_err(dev, "%s: Inval linux,code prop\n", pp->name);
			return ERR_PTR(-EINVAL);
		}

		err = of_property_read_u32(pp, "softbutton-min-x", &btn->min_x);
		if (err) {
			dev_err(dev, "%s: Inval min-x prop\n", pp->name);
			return ERR_PTR(-EINVAL);
		}

		err = of_property_read_u32(pp, "softbutton-max-x", &btn->max_x);
		if (err) {
			dev_err(dev, "%s: Inval max-x prop\n", pp->name);
			return ERR_PTR(-EINVAL);
		}

		err = of_property_read_u32(pp, "softbutton-min-y", &btn->min_y);
		if (err) {
			dev_err(dev, "%s: Inval min-y prop\n", pp->name);
			return ERR_PTR(-EINVAL);
		}

		err = of_property_read_u32(pp, "softbutton-max-y", &btn->max_y);
		if (err) {
			dev_err(dev, "%s: Inval max-y prop\n", pp->name);
			return ERR_PTR(-EINVAL);
		}

		err = of_property_read_string(pp, "linux,led-trigger",
					      &btn->ledtrigger_name);
		if (err)
			continue; /* The LED trigger is optional */

		/* Check if another softbutton uses the same trigger */
		for (j = 0; j < i; j++) {
			if (info->buttons[j].ledtrigger_name &&
			    strcmp(info->buttons[j].ledtrigger_name,
				   btn->ledtrigger_name) == 0) {
				btn->ledtrigger = info->buttons[j].ledtrigger;
				break;
			}
		}
		if (!btn->ledtrigger) {
			btn->ledtrigger =
				devm_kzalloc(dev, sizeof(*btn->ledtrigger),
					     GFP_KERNEL);
			if (!btn->ledtrigger)
				return ERR_PTR(-ENOMEM);

			btn->ledtrigger->name = btn->ledtrigger_name;
			err = devm_led_trigger_register(dev, btn->ledtrigger);
			if (err) {
				dev_err(dev, "%s: Ledtrigger register error\n",
					pp->name);
				return ERR_PTR(err);
			}
		}
	}

	__set_bit(EV_KEY, input->evbit);
	for (i = 0; i < info->button_count; i++)
		__set_bit(info->buttons[i].keycode, input->keybit);

	return info;
}

/**
 * touchscreen_handle_softbuttons - check for softbutton press
 * @info: softbutton info retured by devm_touchscreen_alloc_softbuttons.
 *
 * This function checks if the passed in coordinates match any softbutton,
 * and when they do reports a key press / release for the softbutton.
 *
 * Returns true if the coordinates match a softbutton and a key press / release
 * was reported, false otherwise.
 */
bool touchscreen_handle_softbuttons(struct touchscreen_softbutton_info *info,
				    unsigned int x, unsigned int y, bool down)
{
	int i;
	unsigned long led_delay = 1000; /* Keep the led on 1s after release */

	if (info == NULL)
		return false;

	for (i = 0; i < info->button_count; i++) {
		if (x >= info->buttons[i].min_x &&
		    x <= info->buttons[i].max_x &&
		    y >= info->buttons[i].min_y &&
		    y <= info->buttons[i].max_y) {
			input_report_key(info->input,
					 info->buttons[i].keycode, down);

			if (info->buttons[i].ledtrigger && down) {
				led_trigger_event(info->buttons[i].ledtrigger,
						  LED_FULL);
			} else if (info->buttons[i].ledtrigger && !down) {
				/* Led must be off before calling blink */
				led_trigger_event(info->buttons[i].ledtrigger,
						  LED_OFF);
				led_trigger_blink_oneshot(
					info->buttons[i].ledtrigger,
					&led_delay, &led_delay, 0);
			}

			return true;
		}
	}

	return false;
}
