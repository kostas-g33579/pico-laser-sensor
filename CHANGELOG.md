# Changelog

All notable changes to the Pico Laser Particle Counter project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.0.0] - 2025-06-29

### Added
- Complete particle counting system with automatic calibration
- Real-time particle detection using laser beam interruption
- Dual sensor setup for measurement redundancy
- WiFi connectivity with automatic data transmission
- Web-based dashboard for live particle concentration monitoring
- Comprehensive data logging and analysis
- Quality assessment and measurement validation
- CSV data export functionality
- Human-readable analysis reports
- Configurable detection parameters
- Support for different particle size ranges
- Automatic baseline calibration with noise analysis
- Event validation to filter electrical noise and air bubbles
- Multi-format data storage (CSV, JSON, logs)

### Technical Features
- High-frequency sampling (2ms intervals) for accurate detection
- Moving average filtering for noise reduction
- Duration-based particle event validation
- Concentration calculation in particles per minute
- Signal stability monitoring
- False positive detection and filtering
- Quality metrics for measurement reliability

### Server Components
- PHP-based data reception API
- Real-time dashboard with particle concentration display
- Historical data visualization
- Automatic sample classification (Clean, Low, Moderate, High density)
- Status monitoring endpoint
- Backward compatibility with voltage monitoring mode

### Development Tools
- VS Code integration with build tasks
- Comprehensive documentation
- Example configurations for different setups
- Troubleshooting guides

### Changed from v0.1.0
- **Breaking Change**: Migrated from voltage monitoring to particle counting
- Improved data format with structured JSON API
- Enhanced web dashboard with analysis capabilities
- Added comprehensive calibration system
- Implemented quality assessment metrics
- Upgraded from simple voltage logging to scientific measurement analysis

### Infrastructure
- Updated build system for particle counting features
- Enhanced networking stack with better error handling
- Improved data persistence with multiple file formats
- Added configuration validation and system health checks

### Documentation
- Complete README with installation and usage instructions
- API reference documentation
- Technical specifications and performance metrics
- Troubleshooting guide with common issues
- Contributing guidelines

## [0.1.0] - 2025-06-22

### Added
- Initial voltage monitoring system
- Basic WiFi connectivity
- Simple data transmission to PHP server
- Raw voltage dashboard
- CSV data logging
- Laser and photodiode initialization
- VS Code project configuration

### Features
- Real-time voltage monitoring from two photodiodes
- WiFi-based data transmission every 5 seconds
- Simple web dashboard showing voltage levels
- Basic data persistence in CSV format
- Manual laser control and sensor reading

### Known Issues
- No particle detection logic
- Limited data analysis capabilities
- Basic error handling
- Manual calibration required

---

## Migration Guide: v0.1.0 to v1.0.0

### Breaking Changes
1. **Data Format**: JSON structure changed to include particle counts instead of raw voltages
2. **Calibration**: Automatic calibration replaces manual baseline setting
3. **Detection Logic**: Event-based particle counting replaces continuous voltage monitoring
4. **Dashboard**: Particle concentration display replaces voltage graphs

### Migration Steps
1. Update firmware with new particle counting code
2. Replace server receive_data.php with particle counting version
3. Update dashboard (index.html) to new particle analysis interface
4. Recalibrate system with clean sample
5. Test particle detection with known samples

### Data Compatibility
- Legacy voltage data files are preserved
- New particle counting creates separate data files
- Both systems can run simultaneously for validation
