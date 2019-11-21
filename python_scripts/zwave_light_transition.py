entity_id = data.get('entity_id')
brightness = data.get('brightness')
node_id = hass.states.get(entity_id).attributes['node_id']

service_data = {'node_id': node_id, 'parameter': 8, 'value': 220}
hass.services.call('zwave', 'set_config_parameter', service_data, True)

logger.error(f'Setting node {node_id}')

time.sleep(0.2)

if brightness < 1:
    hass.services.call('light', 'turn_off', {'entity_id': entity_id})
else:
    hass.services.call('light', 'turn_on', {'entity_id': entity_id, 'brightness': brightness})

service_data = {'node_id': node_id, 'parameter': 8, 'value': 10}
hass.services.call('zwave', 'set_config_parameter', service_data, True)
