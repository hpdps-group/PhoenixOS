FROM nvidia/cuda:12.1.0-cudnn8-devel-ubuntu20.04 as base
# FROM zobinhuang/pytorch:1.13.1-devel as base


WORKDIR /root


ARG DEBIAN_FRONTEND=noninteractive


# install dependencies
RUN apt-get update
RUN apt-get install -y libibverbs-dev libboost-all-dev net-tools            \
    git-lfs pkg-config python3-pip libelf-dev libssl-dev libgl1-mesa-dev    \
    libvdpau-dev iputils-ping wget gdb vim nsight-compute-2023.1.1

RUN ln -s /opt/nvidia/nsight-compute/2023.1.1/target/linux-desktop-glibc_2_11_3-x64/ncu /usr/local/bin/ncu

RUN python3 -m pip install meson
RUN python3 -m pip install ninja
RUN python3 -m pip install cmake
RUN python3 -m pip install -U matplotlib seaborn palettable panda numpy

# install nsys-cli for profiling
COPY ./dockerfiles/assets/NsightSystems-linux-cli-public-2023.4.1.97-3355750.deb /tmp
RUN cd /tmp && \
    dpkg -i NsightSystems-linux-cli-public-2023.4.1.97-3355750.deb && \
    rm -rf /tmp/NsightSystems-linux-cli-public-2023.4.1.97-3355750.deb


# FROM base as final
# WORKDIR /root
# # copy the root directory of POS to the container
# # make sure the context is located at phoenixos/samples/cuda_resnet_train_migration
# COPY .. /root/
# # build pos
# RUN bash build.sh -t cuda -c
# RUN bash build.sh -t cuda -j -u true

# run server
