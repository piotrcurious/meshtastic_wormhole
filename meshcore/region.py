import math

class Region:
    """
    Represents a bounded geographical region.
    Uses Haversine formula to compute distance from center to verify coordinates membership.
    """
    def __init__(self, name: str, center_lat: float, center_lon: float, radius_meters: float):
        self.name = name
        self.center_lat = center_lat
        self.center_lon = center_lon
        self.radius_meters = radius_meters

    def distance_to(self, lat: float, lon: float) -> float:
        """
        Computes the Haversine distance in meters to a given (lat, lon) coordinate.
        """
        # Earth radius in meters
        R = 6371000.0

        phi1 = math.radians(self.center_lat)
        phi2 = math.radians(lat)
        delta_phi = math.radians(lat - self.center_lat)
        delta_lambda = math.radians(lon - self.center_lon)

        a = (math.sin(delta_phi / 2.0) ** 2 +
             math.cos(phi1) * math.cos(phi2) * (math.sin(delta_lambda / 2.0) ** 2))

        c = 2.0 * math.atan2(math.sqrt(a), math.sqrt(1.0 - a))
        return R * c

    def contains(self, lat: float, lon: float) -> bool:
        """
        Checks if the coordinate (lat, lon) is within the region radius boundary.
        """
        return self.distance_to(lat, lon) <= self.radius_meters

    def __repr__(self):
        return f"<Region name={self.name} center=({self.center_lat}, {self.center_lon}) radius={self.radius_meters}m>"
