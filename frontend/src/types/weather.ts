/* Weather types */

export interface WeatherStatus {
  enabled: boolean
  severe?: boolean
  simulated?: boolean
  reasons?: string
  latitude?: number
  longitude?: number
  alert_zones?: string
  wind_threshold_mph?: number
  poll_interval?: number
  control_register?: string
}

export interface WeatherConfigData {
  enabled: boolean
  latitude: number
  longitude: number
  alert_zones: string
  alert_types: string
  wind_speed_mph: number
  severe_keywords: string
  poll_interval: number
  control_register: string
  severe_raw_value?: number
  normal_raw_value?: number
}

export interface WeatherAlert {
  event: string
  headline: string
  severity: string
  urgency: string
  matched: boolean
}

export interface ForecastPeriod {
  name: string
  temperature: number
  wind: string
  wind_direction: string
  short_forecast: string
  detailed_forecast: string
}

export interface WeatherReport {
  alerts: WeatherAlert[]
  forecast: ForecastPeriod[]
}
