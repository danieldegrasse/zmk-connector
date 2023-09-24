#include <stdio.h>
#include <wchar.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <hidapi/hidapi.h>
#include <cjson/cJSON.h>

#ifdef __clang__
#define __packed __attribute__((packed))
#else
#error "Unsupported compiler"
#endif

#define MAX_STR 255

/* ZMK Keyboard vendor and product ID */
#define ZMK_VEND_ID 0x1d50
#define ZMK_PROD_ID 0x615e

#define SETTINGS_REPORT_ID_FUNCTIONS 0x3
#define SETTINGS_REPORT_ID_KEY_SEL 0x4
#define SETTINGS_REPORT_ID_KEY_DATA 0x5
#define SETTINGS_REPORT_ID_KEY_COMMIT 0x6

/* ZMK HID report structures */
struct zmk_hid_vendor_functions_report_body {
    /* Number of keys on this keyboard */
    uint8_t keycount;
    /* Number of layers supported/used by this keyboard */
    uint8_t layers;
    /* Protocol revision */
    uint8_t protocol_rev;
    /* Flag to indicate dynamic keymap support */
    uint8_t key_remap_support : 1;
    /* Flags reserved for future functionality */
    uint8_t reserved : 7;
} __packed;

struct zmk_hid_vendor_functions_report {
    uint8_t report_id;
    struct zmk_hid_vendor_functions_report_body body;
} __packed;


struct zmk_hid_vendor_key_sel_report_body {
    uint8_t layer_index;
    uint8_t key_index;
} __packed;

struct zmk_hid_vendor_key_sel_report {
    uint8_t report_id;
    struct zmk_hid_vendor_key_sel_report_body body;
} __packed;

struct zmk_hid_vendor_key_data_report_body {
    uint32_t behavior_id;
    uint32_t param1;
    uint32_t param2;
} __packed;

struct zmk_hid_vendor_key_data_report {
    uint8_t report_id;
    struct zmk_hid_vendor_key_data_report_body body;
} __packed;

struct zmk_hid_vendor_key_commit_report {
    uint8_t report_id;
} __packed;

/* Helper to convert UTF16 to UTF8, and add string to cJSON object */
static cJSON* add_utf16_to_object(cJSON *object, char *name, wchar_t *str)
{
	char utf8_str[256];
	int len, wchar_len, wchar_idx, utf8_idx;

	wchar_len = wcslen(str);
	utf8_idx = wchar_idx = 0;
	/* This loop converts the wchar_t string to utf8 formatted char */
	while ((wchar_idx < wchar_len) && (utf8_idx < sizeof(utf8_str))) {
		/* Convert one wchat_t to char */
		len = wctomb(&utf8_str[utf8_idx], str[wchar_idx]);
		if (len == -1) {
			/* Cannot decode wchar to current locale */
			return NULL;
		}
		/* Advance wchar and utf8 index*/
		utf8_idx += len;
		wchar_idx++;
	}
	/* Reset wctomb state, null terminate utf8 string */
	wctomb(NULL, 0);
	utf8_str[utf8_idx] = '\0';

	return cJSON_AddStringToObject(object, name, utf8_str);
}

/* Helper function to open HID device by the serial number */
static hid_device *open_device(char *serial)
{
	wchar_t serial_wchar[256];
	int utf8_idx, wchar_idx, utf8_len, len;

	/* Convert utf8 serial to wchar serial */
	utf8_idx = wchar_idx = 0;
	utf8_len = strlen(serial);
	while ((utf8_idx < utf8_len) && (wchar_idx < 256)) {
		len = mbtowc(&serial_wchar[wchar_idx], &serial[utf8_idx],
			(utf8_len - utf8_idx));
		if (len == -1) {
			printf("Could not convert to UTF-8 str\n");
			return NULL;
		}
		utf8_idx += len;
		wchar_idx++;
	}
	/* Reset mbtowc state */
	mbtowc(NULL, NULL, 0);
	/* Null terminate wchar */
	serial_wchar[wchar_idx] = 0x0;

	/* Open HID device */
	return hid_open(ZMK_VEND_ID, ZMK_PROD_ID, serial_wchar);
}

/**
 * @brief Returns JSON array of all connected HID devices that support ZMK
 *
 * @returns allocated JSON string with all HID devices, or NULL.
 * 	If string is returned, caller must free it.
 */
char *get_device_list(void)
{
	struct hid_device_info *zmk_device, *device_head, *prev_device;
	cJSON *device_array, *device_obj;
	char *json_str;

	device_head = hid_enumerate(ZMK_VEND_ID, ZMK_PROD_ID);
	if (device_head == NULL) {
		printf("Could not enumerate devices: %ls\n", hid_error(NULL));
		return NULL;
	}
	zmk_device = device_head;
	prev_device = NULL;
	device_array = cJSON_CreateArray();
	if (device_array == NULL) {
		goto out;
	}
	while (zmk_device) {
		if (prev_device == NULL || wcscmp(zmk_device->serial_number, prev_device->serial_number)) {
			device_obj = cJSON_CreateObject();
			if (device_obj == NULL) {
				goto out;
			}
			if (add_utf16_to_object(device_obj, "manufacturer",
					zmk_device->manufacturer_string) == NULL) {
				goto out;
			}
			if (add_utf16_to_object(device_obj, "product",
					zmk_device->product_string) == NULL) {
				goto out;
			}
			if (add_utf16_to_object(device_obj, "serial",
					zmk_device->serial_number) == NULL) {
				goto out;
			}
			cJSON_AddItemToArray(device_array, device_obj);
		}
		prev_device = zmk_device;
		zmk_device = zmk_device->next;
	}
	hid_free_enumeration(device_head);
	json_str = cJSON_PrintUnformatted(device_array);
	if (json_str == NULL) {
		goto out;
	}
	cJSON_Delete(device_array);
	return json_str;
out:
	/* Error case, free enumeration and delete cJSON object */
	printf("JSON error while processing device list\n");
	hid_free_enumeration(device_head);
	cJSON_Delete(device_array);
	return NULL;
}

/*
 * Gets keyboard features as a JSON string
 *
 * @param serial: string with serial number
 * @return JSON string with keyboard feature data, or NULL. If a string is
 * 	returned, the caller must free it.
 */
char *get_keyboard_features(char *serial)
{
	struct zmk_hid_vendor_functions_report report;
	hid_device *dev;
	int res;
	char *json_str;
	cJSON *features;

	/* Open HID device */
	dev = open_device(serial);
	if (dev == NULL) {
		printf("Unable to open device: %ls\n", hid_error(NULL));
 		return NULL;
	}
	report.report_id = SETTINGS_REPORT_ID_FUNCTIONS;
	res = hid_get_feature_report(dev, (uint8_t *)&report, sizeof(report));
	if (res == -1) {
		printf("HID error: %ls\n", hid_error(dev));
		hid_close(dev);
		return NULL;
	}
	/* Parse feature report into JSON */
	features = cJSON_CreateObject();
	if (features == NULL) {
		goto out;
	}
	if (!cJSON_AddNumberToObject(features, "protocol_revision", report.body.protocol_rev)) {
		goto out;
	}
	if (!cJSON_AddNumberToObject(features, "keycount", report.body.keycount)) {
		goto out;
	}
	if (!cJSON_AddNumberToObject(features, "layer_count", report.body.layers)) {
		goto out;
	}
	if (!cJSON_AddBoolToObject(features, "key_remap_support", report.body.key_remap_support)) {
		goto out;
	}
	/* Print JSON object to string */
	json_str = cJSON_PrintUnformatted(features);
	if (json_str == NULL) {
		goto out;
	}
	cJSON_Delete(features);
	hid_close(dev);
	return json_str;
out:
	printf("Error in JSON object construction\n");
	hid_close(dev);
	cJSON_Delete(features);
	return NULL;
}

/**
 * @brief Get JSON key data for a given key
 *
 * @param serial: string with device serial number
 * @param layer: layer index to read key from
 * @param key_idx: key index to read
 * @return string with key data, or NULL on error.
 * 	If string is returned, caller must free it.
 */
char *get_key_data(char *serial, uint8_t layer, uint8_t key_idx) {
        struct zmk_hid_vendor_key_sel_report sel;
        struct zmk_hid_vendor_key_data_report data;
	hid_device *dev;
	char *json_str;
	cJSON *key;
	int res;

	dev = open_device(serial);
	if (dev == NULL) {
		printf("Could not open device: %ls\n", hid_error(dev));
		return NULL;
	}
	key = cJSON_CreateObject();
	if (key == NULL) {
		goto out;
	}
	sel.report_id = SETTINGS_REPORT_ID_KEY_SEL;
	sel.body.layer_index = layer;
	sel.body.key_index = key_idx;
	res = hid_send_feature_report(dev, (uint8_t *)&sel, sizeof(sel));
	if (res == -1) {
		printf("HID error: %ls\n", hid_error(dev));
		goto out;
	}
	/* Read key data */
	data.report_id = SETTINGS_REPORT_ID_KEY_DATA;
	res = hid_get_feature_report(dev, (uint8_t *)&data, sizeof(data));
	if (res == -1) {
		printf("HID error: %ls\n", hid_error(dev));
		goto out;
	}
	/* Pass key data to JSON */
	if (!cJSON_AddNumberToObject(key, "behavior_id", data.body.behavior_id)) {
		goto out;
	}
	if (!cJSON_AddNumberToObject(key, "param1", data.body.param1)) {
		goto out;
	}
	if (!cJSON_AddNumberToObject(key, "param2", data.body.param2)) {
		goto out;
	}
	json_str = cJSON_PrintUnformatted(key);
	if (json_str == NULL) {
		goto out;
	}

	cJSON_Delete(key);
	hid_close(dev);
	return json_str;
out:
	printf("Error while reading key data\n");
	hid_close(dev);
	cJSON_Delete(key);
	return NULL;
}


void print_help(void) {
	printf("zmk_connector: Interact with ZMK keyboard\n");
	printf("commands: \n");
	printf("\tlist_devices: list all connected devices in a JSON array\n");
	printf("\tread_features <serial>: read keyboard with serial <serial> features\n");
	printf("\tread_key <serial> <layer> <key_idx>: read key data from keyboard with <serial>\n"
		"\t\tin layer <layer>, key number <key_idx>\n");
	return;
}

int main(int argc, char* argv[])
{
	int res;
	char *hid_string;
	/* Initialize the hidapi library */
	res = hid_init();
	if (res != 0) {
		printf("Failed to open HIDAPI: %ls\n", hid_error(NULL));
		return res;
	}
	/* Parse helper arguments */
	if (argc == 1) {
		print_help();
		return -EINVAL;
	}
	if (strcmp(argv[1], "help") == 0) {
		print_help();
		return 0;
	} else if (strcmp(argv[1], "list_devices") == 0) {
		hid_string = get_device_list();
		if (hid_string == NULL) {
			printf("Could not get device list\n");
			return -EIO;
		}
		printf("%s\n", hid_string);
		free(hid_string);
		return 0;
	} else if (strcmp(argv[1], "read_features") == 0) {
		if (argc != 3) {
			printf("Error, device serial required\n");
			return -EINVAL;
		}
		hid_string = get_keyboard_features(argv[2]);
		if (hid_string == NULL) {
			printf("Could not get keyboard features\n");
			return -EIO;
		}
		printf("%s\n", hid_string);
		free(hid_string);
		return 0;
	} else if (strcmp(argv[1], "read_key") == 0) {
		int layer, key;

		if (argc != 5) {
			printf("Error, device serial, layer, and key required\n");
			return -EINVAL;
		}
		layer = strtol(argv[3], NULL, 10);
		key = strtol(argv[4], NULL, 10);

		hid_string = get_key_data(argv[2], layer, key);
		if (hid_string == NULL) {
			printf("Could not get key data\n");
			return -EIO;
		}
		printf("%s\n", hid_string);
		free(hid_string);
		return 0;
	}
	printf("Invalid arguments\n");
	return -EINVAL;
}
