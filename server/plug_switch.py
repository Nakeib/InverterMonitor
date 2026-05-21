import argparse
import json
import os
import tinytuya

def parse_switch_state(value):
    normalized = value.strip().lower()
    if normalized in ('1', 'true', 'on', 'yes'):
        return True
    if normalized in ('0', 'false', 'off', 'no'):
        return False
    try:
        if '.' in normalized:
            return float(normalized)
        return int(normalized)
    except ValueError:
        return value


def load_relay_config(config_path):
    with open(config_path, 'r', encoding='utf-8') as config_file:
        return json.load(config_file)


def get_device_id(relays, index):
    for relay in relays:
        if int(relay.get('index')) == index:
            return relay.get('id')
    raise ValueError(f'Device index {index} not found in relays configuration')


def main():
    parser = argparse.ArgumentParser(description='Send a switch command via Tinytuya Cloud.')
    parser.add_argument('--index', dest='device_index', type=int, required=True,
                        help='Index of the target device in relays.json')
    parser.add_argument('--state', dest='switch_state', required=True,
                        help='Switch state value to send (True/False, on/off, 1/0, or raw value)')
    args = parser.parse_args()

    config_path = os.path.join(os.path.dirname(__file__), 'relays.json')
    relay_config = load_relay_config(config_path)

    api_region = relay_config.get('apiRegion')
    api_key = relay_config.get('apiKey')
    api_secret = relay_config.get('apiSecret')
    device_id = get_device_id(relay_config.get('relays', []), args.device_index)
    switch_state = parse_switch_state(args.switch_state)

    print('Initialize tinytuya.Cloud...')
    c = tinytuya.Cloud(
        apiRegion=api_region,
        apiKey=api_key,
        apiSecret=api_secret,
        apiDeviceID=device_id)

    commands = {
        'commands': [{
            'code': 'switch_1',
            'value': switch_state
        }]
    }

    print('Sending command...')
    result = c.sendcommand(device_id, commands)
    print('Results\n:', result)


if __name__ == '__main__':
    main()
