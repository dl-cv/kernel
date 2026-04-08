Kernel driver pwm-fan
=====================

This driver enables the use of a PWM module to drive a fan. It uses the
generic PWM interface thus it is hardware independent. It can be used on
many SoCs, as long as the SoC supplies a PWM line driver that exposes
the generic PWM API.

Author: Kamil Debski <k.debski@samsung.com>

Description
-----------

The driver implements a simple interface for driving a fan connected to
a PWM output. It uses the generic PWM interface, thus it can be used with
a range of SoCs. The driver exposes the fan to the user space through
the hwmon's sysfs interface.

The fan rotation speed returned via the optional 'fan1_input' is extrapolated
from the sampled interrupts from the tachometer signal within 1 second.

Sysfs entries
-------------

The driver exposes standard hwmon attributes under ``/sys/class/hwmon/hwmonX/``:

* ``pwm1`` (RW): PWM duty in the range 0..255
* ``fan1_input`` (RO, optional): measured fan RPM when tachometer IRQ exists

The following control attributes are also exported:

* ``manual_mode`` (RW): when set to ``1``, manual writes to ``pwm1`` are kept
  and thermal notifier updates are ignored. Set back to ``0`` to return to
  automatic thermal control.

On Rockchip platforms using ``rockchip,temp-trips`` with
``CONFIG_ROCKCHIP_SYSTEM_MONITOR``, profile switching is available:

* ``fan_mode`` (RW): numeric profile selector
  ``0=silent``, ``1=normal``, ``2=turbo``
* ``fan_mode_name`` (RW): string profile selector
  ``silent`` / ``normal`` / ``turbo``
* ``fan_mode_names`` (RO): all supported profile names

When changing ``fan_mode`` / ``fan_mode_name`` in automatic mode
(``manual_mode=0``), the driver reapplies PWM immediately using the latest
temperature sample from system monitor.
