FROM ubuntu:22.04

# Install dependencies
RUN apt-get update && apt-get install -y \
    git \
    cmake \
    gcc-arm-none-eabi \
    build-essential \
    php \
    php-cli \
    curl \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /workspace

# Expose port for PHP server
EXPOSE 8000

CMD ["/bin/bash"]
