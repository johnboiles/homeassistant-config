wd200_nodes = [7, 8, 9, 10, 11, 12]
parameters = {
    7: 1,
    8: 10,
    9: 1,
    10: 10,
}

for node in wd200_nodes:
    for parameter in parameters:
        service_data = {'node_id': node, 'parameter': parameter, 'value': parameters[parameter]}
        hass.services.call('ozw', 'set_config_parameter', service_data, True)
        time.sleep(.2)
