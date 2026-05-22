import h5py
import imagecodecs
import numpy

import hdf5plugin

codeformat = "J2K"  # 'J2K' or 'JP2

data = numpy.random.randint(3000, size=(10, 512, 1024), dtype=numpy.uint16)
chunks = (1,) + data.shape[1:]


print("* hdf5plugin roundtrip (lossless)")


with h5py.File("test.h5", "w") as f:
    f.create_dataset(
        "data", data=data, chunks=chunks, compression=hdf5plugin.Jpeg2000()
    )

with h5py.File("test.h5", "r") as f:
    retrieved = f["data"][()]

assert numpy.array_equal(data, retrieved)


print("* hdf5plugin roundtrip (lossy)")

for compression_ratio in [5.0, 10.0]:
    print(f"* hdf5plugin roundtrip (lossy, compression ratio: {compression_ratio})")
    with h5py.File("test.h5", "w") as f:
        f.create_dataset(
            "data",
            data=data,
            chunks=chunks,
            compression=hdf5plugin.Jpeg2000(compression_ratio),
        )

    with h5py.File("test.h5", "r") as f:
        dataset = f["data"]
        retrieved = dataset[()]
        compressed_size = dataset.id.get_storage_size()

        print("  - Filters:")
        create_plist = dataset.id.get_create_plist()
        for index in range(create_plist.get_nfilters()):
            filter_id, _, filter_options, _ = create_plist.get_filter(index)
            print(
                f"  - Filter ID: {filter_id}, Options: {filter_options}, Class: {hdf5plugin.from_filter_options(filter_id, filter_options)}"
            )

    print(f"  - effective compression ratio: {data.nbytes / compressed_size}")


print("* Write with hdf5plugin, direct chunk read (lossless)")


with h5py.File("test_j2k_direct_chunk.h5", "w") as f:
    f.create_dataset(
        "data", data=data, chunks=chunks, compression=hdf5plugin.Jpeg2000()
    )


with h5py.File("test_j2k_direct_chunk.h5", "r") as f:
    dataset = f["data"]
    direct_chunk_read_data = numpy.empty_like(dataset)
    for index in range(dataset.id.get_num_chunks()):
        filter_mask, chunk = dataset.id.read_direct_chunk(
            dataset.id.get_chunk_info(index).chunk_offset
        )
        direct_chunk_read_data[index] = imagecodecs.jpeg2k_decode(chunk).astype(
            dataset.dtype
        )


assert numpy.array_equal(data, direct_chunk_read_data)


print("* Direct chunk write, read with hdf5plugin (lossless)")


with h5py.File("test_j2k_direct_chunk.h5", "w") as f:
    dataset = f.create_dataset(
        "data",
        shape=data.shape,
        dtype=data.dtype,
        chunks=(1,) + data.shape[1:],
        compression=hdf5plugin.Jpeg2000(),
    )
    for index in range(len(dataset)):
        dataset.id.write_direct_chunk(
            (index, 0, 0),
            imagecodecs.jpeg2k_encode(data[index], level=0, codecformat=codeformat),
        )


with h5py.File("test_j2k_direct_chunk.h5", "r") as f:
    hdf5plugin_read_data = f["data"][()]


assert numpy.array_equal(data, hdf5plugin_read_data)
