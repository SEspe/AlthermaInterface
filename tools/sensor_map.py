"""Sensor locations and the placement relations they must satisfy.

Edit this file when a sensor is moved, renamed or reassigned to another node.
sensor_check.py reads it and reports whether the data still matches the
placement, so a wrong entry shows up as CHECK instead of a silent mismatch.

Each SENSORS entry:
  entity        Home Assistant entity id
  location      where the sensor sits (see docs/system_sensors.svg)
  source        "X10A" (read by the Altherma unit) or the ESPEasy node
  offset_before optional (datetime, offset): correction applied to readings
                taken before that time, for sensors calibrated on the node
"""
from datetime import datetime

from ha_baseline import LOCAL

HOST = "http://192.168.10.151:8123"

SENSORS = {
    "refrig": {"entity": "sensor.espaltherma_refrig_temp_liquid_sidec",
               "location": "refrigerant liquid, indoor unit", "source": "X10A"},
    "inlet": {"entity": "sensor.espaltherma_inlet_water_tempc",
              "location": "heat pump water inlet", "source": "X10A"},
    "outlet": {"entity": "sensor.espaltherma_outlet_water_tempc",
               "location": "heat pump water outlet", "source": "X10A"},
    "dhw": {"entity": "sensor.espaltherma_dhw_tank_tempc",
            "location": "DHW return line, from tank", "source": "X10A"},
    "turmaskin": {"entity": "sensor.turmaskin",
                  "location": "heat pump flow, next to outlet", "source": "ESPEasy .160"},
    "returmaskin": {"entity": "sensor.returmaskin",
                    "location": "heat pump return, next to inlet", "source": "ESPEasy .160"},
    "turcomp": {"entity": "sensor.turcompressor",
                "location": "compressor line to (7a)", "source": "ESPEasy .160 (confirm)"},
    "returcomp": {"entity": "sensor.returcompressor",
                  "location": "compressor return to indoor unit (7b)", "source": "ESPEasy .160 (confirm)"},
    "lager_in": {"entity": "sensor.tempinnlagertank",
                 "location": "buffer top, heat pump supply in", "source": "ESPEasy (confirm)"},
    "turlager": {"entity": "sensor.turlager",
                 "location": "buffer top, supply to consumers", "source": "ESPEasy (confirm)"},
    "lager_ut": {"entity": "sensor.temputlagertank",
                 "location": "buffer bottom, return to indoor unit", "source": "ESPEasy (confirm)"},
    "ford_ut": {"entity": "sensor.temputfordeler",
                "location": "manifold, house flow", "source": "ESPEasy .162 (confirm)"},
    "ford_in": {"entity": "sensor.tempreturfordeler",
                "location": "house return", "source": "ESPEasy .162 (confirm)"},
    "gulv_til": {"entity": "sensor.tilfordelergulv",
                 "location": "floor supply after mixing valve", "source": "ESPEasy .162 (confirm)"},
    "gulv_fra": {"entity": "sensor.frafordelergulv",
                 "location": "floor return", "source": "ESPEasy .162 (confirm)"},
    "ute": {"entity": "sensor.tempute",
            "location": "outdoor air", "source": "ESPEasy .160"},
    "stue": {"entity": "sensor.tempstue",
             "location": "living room, DS18B20",
             "source": "ESPEasy .161",
             "offset_before": (datetime(2026, 10, 23, 16, 22, tzinfo=LOCAL), -2.0)},
    "stue2": {"entity": "sensor.tempstue2",
              "location": "living room, BME280",
              "source": "ESPEasy .161",
              "offset_before": (datetime(2026, 10, 24, 7, 51, tzinfo=LOCAL), -2.9)},
    "garasje": {"entity": "sensor.tempgarasje",
                "location": "garage, not on the water loop", "source": "ESPEasy"},
}

BINARY = {
    "comp": "binary_sensor.espaltherma_compressor",
    "dhw": "binary_sensor.espaltherma_priority_to_domestic_water",
    "htr": "binary_sensor.espaltherma_electric_heater_contactor",
}

# (a, b, relation, tolerance, note)
#   same        |mean(a-b)| <= tolerance, spread <= 0.5 K, and a and b respond
#               to compressor transitions within 1 minute of each other
#   same_level  |mean(a-b)| <= tolerance and spread <= 0.5 K only; for sensors
#               the compressor does not move quickly (e.g. living room)
#   warmer      mean(a-b) >= 0.3 K while compressing
PAIRS = [
    ("inlet", "returmaskin", "same", 1.0, "heat pump inlet and return, close together"),
    ("outlet", "turmaskin", "same", 1.0, "heat pump outlet and flow, close together"),
    ("turlager", "ford_ut", "same", 0.5, "same pipe, no loss between them"),
    ("turlager", "lager_ut", "warmer", None, "buffer top warmer than bottom while compressing"),
    ("lager_in", "turlager", "warmer", None, "buffer inlet warmer than supply to consumers"),
    ("returmaskin", "lager_ut", "same", 1.0, "heat pump return and buffer return, same water"),
    ("ford_ut", "gulv_til", "warmer", None, "mixing valve cools the floor supply"),
    ("stue", "stue2", "same_level", 0.5, "living room sensors, both corrected"),
]
