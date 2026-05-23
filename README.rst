hdf5plugin
==========

.. image:: https://zenodo.org/badge/DOI/10.5281/zenodo.7257761.svg
   :target: https://doi.org/10.5281/zenodo.7257761

*hdf5plugin* provides `HDF5 compression filters <https://github.com/HDFGroup/hdf5_plugins/blob/master/docs/RegisteredFilterPlugins.md#list-of-filters-registered-with-the-hdf-group>`_ (namely: Blosc, Blosc2, BitShuffle, BZip2, FciDecomp, LZ4, Sperr, SZ, SZ3, Zfp, ZStd) and makes them usable from `h5py <https://www.h5py.org>`_.

See `documentation <http://www.silx.org/doc/hdf5plugin/latest/>`_.

Installation
------------

To install, run::

     pip install hdf5plugin [--user]
     
or, with conda (https://anaconda.org/conda-forge/hdf5plugin)::

    conda install -c conda-forge hdf5plugin

To install from source and recompile the HDF5 plugins, run::

     pip install hdf5plugin --no-binary hdf5plugin [--user]

Installing from source can achieve better performances by enabling AVX2 and OpenMP if available.

For more details, see the `installation documentation <http://www.silx.org/doc/hdf5plugin/latest/install.html>`_.

How-to use
----------

To use it, just use ``import hdf5plugin`` and supported compression filters are available from `h5py <https://www.h5py.org>`_.

For details, see `Usage documentation <http://www.silx.org/doc/hdf5plugin/latest/usage.html>`_.


JPEG2000 backend model
----------------------

This branch adds an experimental ``Jpeg2000`` HDF5 filter with a backend
selection layer.  The HDF5 filter itself is installed as a normal hdf5plugin
filter in::

    hdf5plugin/plugins/libh5jpeg2000.so

Backend implementations are deliberately kept separate from HDF5 filters.  A
backend is not registered with HDF5 directly; it is loaded by the JPEG2000
filter dispatcher when needed.  Optional backends are installed below a
plugin-specific backend directory, for example::

    hdf5plugin/backends/jpeg2000/libh5jpeg2000_kakadu_backend.so

This keeps the file format stable while allowing different sites to use
different JPEG2000 engines.  Files compressed with the JPEG2000 filter do not
record whether OpenJPEG or Kakadu was used.  A reader can use any compatible
backend available in its own process.

The current backend policy is:

* ``openjpeg`` is the built-in fallback backend of ``libh5jpeg2000.so``.
* ``kakadu`` is an optional backend library loaded with ``dlopen``.
* Kakadu libraries are not bundled in the Python package or wheel.
* If Kakadu is selected but its runtime libraries are not reachable through
  ``LD_LIBRARY_PATH``, the backend is not used.
* In ``auto`` mode, the packaged manifest can prefer Kakadu and fall back to
  OpenJPEG when Kakadu cannot be loaded.

Runtime backend selection can be done in Python::

    import hdf5plugin

    hdf5plugin.Jpeg2000.configure_backend("auto")     # manifest/default policy
    hdf5plugin.Jpeg2000.configure_backend("openjpeg") # force OpenJPEG
    hdf5plugin.Jpeg2000.configure_backend("kakadu")   # force Kakadu

or with environment variables::

    export HDF5PLUGIN_JPEG2000_BACKEND=kakadu
    export HDF5PLUGIN_JPEG2000_BACKEND=openjpeg
    export HDF5PLUGIN_JPEG2000_BACKEND=auto

The packaged manifest is ``hdf5plugin_jpeg2000_plugins.json``.  A custom
manifest can be selected with::

    export HDF5PLUGIN_JPEG2000_MANIFEST=/path/to/hdf5plugin_jpeg2000_plugins.json

Kakadu test procedure used in this development environment
----------------------------------------------------------

The following commands assume the same environment used during development:

* source tree: ``/data/scisofttmp/mirone/PROJECTS/segmentation/NR/hdf5plugin_thomas``
* test virtual environment: ``/data/scisofttmp/mirone/environments/hdf5plugin_jpeg2000_test``
* Kakadu installation already present in: ``/data/scisofttmp/mirone/KD``
* OpenJPEG pkg-config from the NightRail development environment

Copy-paste build and install::

    cd /data/scisofttmp/mirone/PROJECTS/segmentation/NR/hdf5plugin_thomas

    source /data/scisofttmp/mirone/environments/hdf5plugin_jpeg2000_test/bin/activate

    export PKG_CONFIG_PATH=/cvmfs/tomo.esrf.fr/software/packages/ubuntu24.04/x86_64/nightraildev/26_06_01/lib/pkgconfig
    export KDIR=/data/scisofttmp/mirone/KD
    export HDF5PLUGIN_STRIP=blosc,blosc2,bshuf,bzip2,fcidecomp,lz4,sperr,sz,sz3,zfp,zstd

    python -m pip install --no-build-isolation --force-reinstall .

Check that the main HDF5 filter does not link Kakadu directly::

    ldd /data/scisofttmp/mirone/environments/hdf5plugin_jpeg2000_test/lib/python3.12/site-packages/hdf5plugin/plugins/libh5jpeg2000.so

The output should list OpenJPEG but not ``libkdu_*``.  The optional backend is
separate and should show unresolved Kakadu libraries unless ``LD_LIBRARY_PATH``
contains the Kakadu lib directory::

    ldd /data/scisofttmp/mirone/environments/hdf5plugin_jpeg2000_test/lib/python3.12/site-packages/hdf5plugin/backends/jpeg2000/libh5jpeg2000_kakadu_backend.so

Run the normal hdf5plugin tests without Kakadu in ``LD_LIBRARY_PATH``.  This
checks that the package remains usable and falls back to OpenJPEG::

    export LD_LIBRARY_PATH=/cvmfs/tomo.esrf.fr/software/packages/ubuntu24.04/x86_64/nightraildev/26_06_01/lib
    python -m unittest hdf5plugin.test

Run an explicit Kakadu roundtrip by adding Kakadu to ``LD_LIBRARY_PATH``::

    export LD_LIBRARY_PATH=/data/scisofttmp/mirone/KD/lib:/cvmfs/tomo.esrf.fr/software/packages/ubuntu24.04/x86_64/nightraildev/26_06_01/lib

    python - <<'PY'
    import os
    import tempfile

    import h5py
    import hdf5plugin
    import numpy as np

    hdf5plugin.Jpeg2000.configure_backend("kakadu")

    path = tempfile.NamedTemporaryFile(suffix=".h5", delete=False).name
    try:
        data = (np.arange(64 * 96, dtype=np.uint16).reshape(64, 96) % 2048)
        with h5py.File(path, "w") as h5:
            h5.create_dataset(
                "data",
                data=data,
                compression=hdf5plugin.Jpeg2000(compression_ratio=1.0),
            )
        with h5py.File(path, "r") as h5:
            out = h5["data"][()]
        np.testing.assert_array_equal(out, data)
        print("kakadu_roundtrip_ok")
    finally:
        os.unlink(path)
    PY

To debug backend selection, enable dispatcher diagnostics::

    export HDF5PLUGIN_JPEG2000_DEBUG=1


License
-------

The source code of *hdf5plugin* itself is licensed under the `MIT license <LICENSE>`_.

Embedded HDF5 compression filters are licensed under different open-source licenses:
see the `license documentation <http://www.silx.org/doc/hdf5plugin/latest/information.html#license>`_.
