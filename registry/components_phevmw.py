# Copyright The Phorepoc Authors
# SPDX-License-Identifier: LGPL-2.1-only

r"""components_phevmw -- the phevmware-tools package's components (merged into core's registry when this repository is built:
PHOREPOC_ADAPTATIONS=<adaptations>:<phevmware-tools>, see the pdk's tools/emitter/paths.py).

  phevmwsvc   the VMware tools' service (package only: recipes/sis.sh packs it)
  phevmwtest  the same with a self-test, a boot-test component for a board image build (SSM_TEST=phevmwtest); not packaged
"""
import os

_HERE = os.path.dirname(os.path.abspath(__file__))
_SRC = os.path.join(_HERE, "..", "os", "src", "phevmwsvc")

COMPONENTS = {
    "phevmwsvc": dict(mmp=os.path.join(_SRC, "phevmwsvc.mmp"), package_only=True, sis=True),
    "phevmwtest": dict(mmp=os.path.join(_SRC, "phevmwtest.mmp"), test=True),
}
