#ifndef REGION_H
#define REGION_H

#include <string>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

class Region {
public:
    std::string name;
    double center_lat;
    double center_lon;
    double radius_meters;

    Region() : name(""), center_lat(0.0), center_lon(0.0), radius_meters(0.0) {}
    Region(std::string name, double lat, double lon, double radius)
        : name(name), center_lat(lat), center_lon(lon), radius_meters(radius) {}

    double distance_to(double lat, double lon) const {
        double R = 6371000.0; // Earth radius in meters
        double phi1 = center_lat * M_PI / 180.0;
        double phi2 = lat * M_PI / 180.0;
        double delta_phi = (lat - center_lat) * M_PI / 180.0;
        double delta_lambda = (lon - center_lon) * M_PI / 180.0;

        double a = sin(delta_phi / 2.0) * sin(delta_phi / 2.0) +
                   cos(phi1) * cos(phi2) * sin(delta_lambda / 2.0) * sin(delta_lambda / 2.0);
        double c = 2.0 * atan2(sqrt(a), sqrt(1.0 - a));
        return R * c;
    }

    bool contains(double lat, double lon) const {
        return distance_to(lat, lon) <= radius_meters;
    }
};

#endif // REGION_H
