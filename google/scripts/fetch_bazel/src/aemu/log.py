# Copyright 2024 - The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
import logging
import os
import platform
import sys


class ColorFormatter(logging.Formatter):
    # See https://chrisyeh96.github.io/2020/03/28/terminal-colors.html

    yellow = "\N{ESC}[33;20m"
    red = "\N{ESC}[31;20m"
    reset = "\N{ESC}[0m"
    msg_fmt = "%(message)s"

    FORMATS = {
        logging.DEBUG: logging.Formatter(msg_fmt),
        logging.INFO: logging.Formatter(msg_fmt),
        logging.WARNING: logging.Formatter(yellow + msg_fmt + reset),
        logging.ERROR: logging.Formatter(red + msg_fmt + reset),
        logging.CRITICAL: logging.Formatter(red + msg_fmt + reset),
    }

    def format(self, record):
        formatter = self.FORMATS.get(record.levelno, logging.Formatter(self.msg_fmt))
        return formatter.format(record)


class LogBelowLevel(logging.Filter):
    """A logging filter that only logs a line if it is below the given level."""

    def __init__(self, exclusive_maximum, name=""):
        super(LogBelowLevel, self).__init__(name)
        self.max_level = exclusive_maximum

    def filter(self, record):
        return True if record.levelno < self.max_level else False


def configure_logging(logging_level):
    """Configures the logging system to log at the given level

    Args:
        logging_level (_type_): A logging level, or number.
    """
    logging_handler_out = logging.StreamHandler(sys.stdout)
    logging_handler_out.setLevel(logging.DEBUG)
    logging_handler_out.addFilter(LogBelowLevel(logging.WARNING))

    logging_handler_err = logging.StreamHandler(sys.stderr)
    logging_handler_err.setLevel(logging.WARNING)

    if sys.stdin and sys.stdin.isatty():
        logging_handler_out.setFormatter(ColorFormatter())
        logging_handler_err.setFormatter(ColorFormatter())

        if platform.system() == "Windows":
            os.system("")  # Activate ansi colors!

    root_logger = logging.getLogger()
    root_logger.setLevel(logging_level)
    root_logger.addHandler(logging_handler_out)
    root_logger.addHandler(logging_handler_err)
