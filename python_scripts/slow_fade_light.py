"""
https://community.home-assistant.io/t/light-fade-in/35509

automation:
- alias: Light 15min Fade to 100% starting an hour before sunset
    trigger:
    platform: sun
    event: sunset
    offset: "-01:00:00"
    action:
    - service: python_script.zwave_fade
        data:
          entity_id: light.level  # Entity ID to fade
          transition: 20  # Seconds to go from current to end brightness
          brightness_pct: 100  # Brightness to end at as a percentage
          brightness: 255  # Brightness to end at (overrides brightness_pct)
"""

entity_id = data.get('entity_id')
brightness = data.get('brightness')
brightness_pct = data.get('brightness_pct')

if entity_id is not None and (brightness is not None or brightness_pct is not None):
    light = hass.states.get(entity_id)

    start_level = light.attributes.get('brightness', 0)
    transition = data.get('transition', 0)

    # Use brightness or convert brightness_pct
    if brightness is not None:
        end_level = int(brightness)
    else:
        end_level = math.ceil(brightness_pct * 2.55)

    # Calculate number of steps
    steps = int(math.fabs((start_level - end_level)))
    if start_level > end_level:
        fadeout = True
    else:
        fadeout = False

    # Calculate the delay time
    delay = round(transition / steps, 3)

    # Disable delay anbd increase stepping if delay < 3/4 second
    if (delay < 0.750):
        delay = 0
        steps = int(steps / 5)
        step_by = 5
    else:
        step_by = 1

    logger.info(f'Setting brightness of {entity_id} from {start_level}' +
        ' to {end_level} steps {steps} delay {delay}')

    new_level = start_level
    for x in range(steps):
        current_level = light.attributes.get('brightness', 0)
        if abs(new_level - current_level) > 2:
            logger.info(f'Current level ({current_level}) does not match expected level ({new_level}), aborting')
            break
        elif fadeout and current_level < new_level:
            logger.info(f'Current level {current_level} < new level {new_level} during fadeout')
            break
        elif not fadeout and current_level > new_level:
            logger.info(f'Current level {current_level} > new level {new_level} during fadein')
            break
        else:
            data = {"entity_id": entity_id, "brightness": new_level}
            hass.services.call('light', 'turn_on', data)
            if fadeout:
                new_level = new_level - step_by
            else:
                new_level = new_level + step_by
            # Do not sleep for 0 delay
            if delay > 0:
                time.sleep(delay)

# Ensure light ends at the final state
if end_level > 0:
    data = {
        "entity_id": entity_id,
        "brightness": end_level
    }
    hass.services.call('light', 'turn_on', data)
else:
    data = {
        "entity_id": entity_id
    }
    hass.services.call('light', 'turn_off', data)
