from esphome import automation, pins
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import fan
from esphome.const import CONF_ENABLE_PIN, CONF_ID, CONF_INVERTED, CONF_PIN

from . import smavista_pulse_cmd_fan_ns

CODEOWNERS = ["@your-github-handle"]
AUTO_LOAD = ["fan"]

SmavistaPulseCmdFan = smavista_pulse_cmd_fan_ns.class_(
    "SmavistaPulseCmdFan", cg.Component, fan.Fan
)
DirectionForwardAction = smavista_pulse_cmd_fan_ns.class_("DirectionForwardAction", automation.Action)
DirectionReverseAction = smavista_pulse_cmd_fan_ns.class_("DirectionReverseAction", automation.Action)
WindAction = smavista_pulse_cmd_fan_ns.class_("WindAction", automation.Action)
WindOffAction = smavista_pulse_cmd_fan_ns.class_("WindOffAction", automation.Action)
SeqSweepAction = smavista_pulse_cmd_fan_ns.class_("SeqSweepAction", automation.Action)

CONF_SHORT_PULSE_US = "short_pulse_us"
CONF_LONG_PULSE_US = "long_pulse_us"
CONF_FRAME_GAP_US = "frame_gap_us"
CONF_REPEAT = "repeat"
CONF_ENABLE_INVERTED = "enable_inverted"
CONF_STOCK_WAVEFORM = "stock_waveform"
CONF_TRAILER_BLIP_US = "trailer_blip_us"
CONF_AUX_PIN = "aux_pin"
CONF_ENABLE_FOLLOW_FAN = "enable_follow_fan"
CONF_ENABLE_LEAD_MS = "enable_lead_ms"
CONF_ENABLE_OFF_DELAY_MS = "enable_off_delay_ms"

CONFIG_SCHEMA = (
    fan.fan_schema(SmavistaPulseCmdFan)
    .extend(
        {
            cv.Required(CONF_PIN): pins.internal_gpio_output_pin_schema,
            # Only for a signal-inverting stage between this pin and the
            # LKS32MC035; this board has none.
            cv.Optional(CONF_INVERTED, default=False): cv.boolean,
            # Full stock waveform: line idles HIGH, and each frame is followed
            # by a trailer. False = bare 32-bit frames, idling LOW.
            cv.Optional(CONF_STOCK_WAVEFORM, default=True): cv.boolean,
            cv.Optional(CONF_TRAILER_BLIP_US, default=100): cv.int_range(
                min=1, max=5000
            ),
            # GPIO20 driven as an output held LOW, as the stock firmware does
            # (purpose unknown).
            cv.Optional(CONF_AUX_PIN): pins.internal_gpio_output_pin_schema,
            # The stock `lowpower_onoff` output (GPIO22): high while the fan is
            # on, raised shortly before the first command and dropped shortly
            # after OFF. See set_enable_pin() in the header.
            cv.Optional(CONF_ENABLE_PIN): pins.internal_gpio_output_pin_schema,
            cv.Optional(CONF_ENABLE_INVERTED, default=False): cv.boolean,
            # False = hold the pin enabled from boot instead of sequencing it.
            cv.Optional(CONF_ENABLE_FOLLOW_FAN, default=True): cv.boolean,
            cv.Optional(CONF_ENABLE_LEAD_MS, default=10): cv.int_range(
                min=0, max=1000
            ),
            cv.Optional(CONF_ENABLE_OFF_DELAY_MS, default=100): cv.int_range(
                min=0, max=10000
            ),
            # Bit timing: each bit is a short and a long pulse.
            cv.Optional(CONF_SHORT_PULSE_US, default=250): cv.positive_int,
            cv.Optional(CONF_LONG_PULSE_US, default=750): cv.positive_int,
            # LOW gap between the repeated copies of each frame.
            cv.Optional(CONF_FRAME_GAP_US, default=8100): cv.positive_int,
            # Copies of each frame per command (stock: 6).
            cv.Optional(CONF_REPEAT, default=6): cv.int_range(min=1, max=20),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await fan.new_fan(config)
    await cg.register_component(var, config)

    pin = await cg.gpio_pin_expression(config[CONF_PIN])
    cg.add(var.set_pin(pin))
    cg.add(var.set_inverted(config[CONF_INVERTED]))
    cg.add(var.set_stock_waveform(config[CONF_STOCK_WAVEFORM]))
    cg.add(var.set_trailer_blip_us(config[CONF_TRAILER_BLIP_US]))
    if CONF_AUX_PIN in config:
        aux_pin = await cg.gpio_pin_expression(config[CONF_AUX_PIN])
        cg.add(var.set_aux_pin(aux_pin))
    if CONF_ENABLE_PIN in config:
        enable_pin = await cg.gpio_pin_expression(config[CONF_ENABLE_PIN])
        cg.add(var.set_enable_pin(enable_pin))
        cg.add(var.set_enable_inverted(config[CONF_ENABLE_INVERTED]))
        cg.add(var.set_enable_follow_fan(config[CONF_ENABLE_FOLLOW_FAN]))
        cg.add(var.set_enable_lead_ms(config[CONF_ENABLE_LEAD_MS]))
        cg.add(var.set_enable_off_delay_ms(config[CONF_ENABLE_OFF_DELAY_MS]))
    cg.add(var.set_short_pulse_us(config[CONF_SHORT_PULSE_US]))
    cg.add(var.set_long_pulse_us(config[CONF_LONG_PULSE_US]))
    cg.add(var.set_frame_gap_us(config[CONF_FRAME_GAP_US]))
    cg.add(var.set_repeat(config[CONF_REPEAT]))


SIMPLE_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.Required(CONF_ID): cv.use_id(SmavistaPulseCmdFan),
    }
)


@automation.register_action(
    "smavista_pulse_cmd_fan.direction_forward", DirectionForwardAction, SIMPLE_ACTION_SCHEMA, synchronous=True
)
async def direction_forward_action_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "smavista_pulse_cmd_fan.direction_reverse", DirectionReverseAction, SIMPLE_ACTION_SCHEMA, synchronous=True
)
async def direction_reverse_action_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "smavista_pulse_cmd_fan.wind", WindAction, SIMPLE_ACTION_SCHEMA, synchronous=True
)
async def wind_action_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "smavista_pulse_cmd_fan.wind_off",
    WindOffAction,
    SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
async def wind_off_action_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)


@automation.register_action(
    "smavista_pulse_cmd_fan.seq_sweep",
    SeqSweepAction,
    SIMPLE_ACTION_SCHEMA,
    synchronous=True,
)
async def seq_sweep_action_to_code(config, action_id, template_arg, args):
    paren = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, paren)
