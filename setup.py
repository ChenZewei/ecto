#!/usr/bin/env python

from distutils.core import setup
from ecto.package import parse_package_for_distutils

d = parse_package_for_distutils()
d['packages'] = ['ecto', 'ecto.sphinx', 'ecto.sphinx.breathe',
                'ecto.sphinx.breathe.finder', 'ecto.sphinx.breathe.finder.doxygen',
                'ecto.sphinx.breathe.parser', 'ecto.sphinx.breathe.parser.doxygen',
                'ecto.sphinx.breathe.renderer', 'ecto.sphinx.breathe.renderer.rst', 'ecto.sphinx.breathe.renderer.rst.doxygen',
               ]
d['package_dir'] = {'': 'python'}
d['install_requires'] = []

setup(**d)
