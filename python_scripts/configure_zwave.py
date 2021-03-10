wd100_entities = [
    'light.living_room_light',
    'light.dining_room_light',
    'light.kitchen_island_light',
    'light.kitchen_light',
    'light.kitchen_hallway_light',
    'light.bedroom_light',
]
parameters = {
    7: 1,
    8: 10,
    9: 1,
    10: 10,
}

for entity in wd100_entities:
    for parameter in parameters:
        service_data = {'entity_id': entity, 'parameter': parameter, 'value': parameters[parameter]}
        hass.services.call('zwave_js', 'set_config_parameter', service_data, True)
        time.sleep(.2)
