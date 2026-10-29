"""Verify the target-specific guard keeps Contracts and Observation independent."""
from pathlib import Path
import tempfile
from core_boundary import verify
source=Path(__file__).resolve().parents[1]
base={'Runtime':'iiSDKCore::Contracts;iiSDKCore::Observation;Threads::Threads',
      'RuntimeInterface':'iiSDKCore::Contracts;iiSDKCore::Observation;Threads::Threads',
      'Contracts':'','Values':'iiSDKCore::Contracts','Observation':'iiSDKCore::Contracts','ObservationInterface':'iiSDKCore::Contracts'}
with tempfile.TemporaryDirectory(prefix='boundary-policy-',dir=source/'build') as temporary:
    root=Path(temporary);fake=root/'source';(fake/'src').mkdir(parents=True);(fake/'cmake').mkdir()
    (fake/'cmake/iiSDKCoreConfig.cmake.in').write_text('find_dependency(Threads)\n')
    build=root/'build';build.mkdir()
    def run(values):
        (build/'core-boundary-Release.txt').write_text(''.join('['+name+']\n'+value+'\n' for name,value in values.items()))
        verify(fake,build)
    run(base)
    for section,dependency in [('Contracts','iiSDKCore::Observation'),('Observation','iiSDKCore::Runtime'),('ObservationInterface','Threads::Threads'),('Runtime','Qt6::Core')]:
        changed=dict(base);changed[section]=dependency
        try:run(changed)
        except RuntimeError:pass
        else:raise RuntimeError('An invalid dependency was accepted: '+section+' -> '+dependency)
    missing=dict(base);missing.pop('Observation')
    try:run(missing)
    except RuntimeError:pass
    else:raise RuntimeError('An incomplete dependency manifest was accepted')
print('Core dependency roles: valid composition accepted, five invalid boundaries rejected')
