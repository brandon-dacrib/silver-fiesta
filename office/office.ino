#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Inkplate.h>
#include <WiFiUdp.h>
#include <esp_sleep.h>
#include "config.h"  // Include your configuration header

#include "FreeMonoBold24pt7b.h"        // Font for status messages
#include "FreeMonoBoldOblique12pt7b.h" // Font for event titles

Inkplate display;
WiFiUDP ntpUDP;
const char* ntpServer = "pool.ntp.org";
time_t lastRefreshTimestamp;

// Function to get local time offset in seconds
int getLocalTimeOffset(time_t timeInput) {
    struct tm timeinfo;
    localtime_r(&timeInput, &timeinfo);

    // Standard time offset in seconds (EST is UTC -5 hours)
    int standardOffset = -4 * 3600;

    // Daylight saving time offset in seconds (EDT is UTC -4 hours)
    int daylightOffset = -4 * 3600;

    // Determine if DST is in effect
    if (timeinfo.tm_isdst > 0) {
        return daylightOffset;
    } else {
        return standardOffset;
    }
}

// Function to calculate battery percentage based on voltage
int getBatteryPercentage(float voltage) {
    float minVoltage = 3.0;  // Define the minimum voltage
    float maxVoltage = 4.2;  // Define the maximum voltage
    int percentage = (int)((voltage - minVoltage) / (maxVoltage - minVoltage) * 100);

    // Ensure the percentage is within bounds
    if (percentage > 100) percentage = 100;
    if (percentage < 0) percentage = 0;

    return percentage;
}

// Function to display battery percentage in the lower right-hand corner
void displayBatteryStatus() {
    float batteryVoltage = display.readBattery();  // Read the battery voltage
    int batteryPercentage = getBatteryPercentage(batteryVoltage);  // Calculate the percentage

    // Display the battery percentage
    //display.setFont(&FreeMonoBoldOblique12pt7b);
    display.setTextSize(2);
    display.setTextColor(INKPLATE_WHITE);

    // Position in the lower right corner
    int batTextX = 550;
    int batTextY = 425;

    // Print the battery percentage
    display.setCursor(batTextX, batTextY);
    display.print(String(batteryPercentage) + "%");

    // Debugging output for voltage and percentage
    Serial.print("Battery Voltage: ");
    Serial.println(batteryVoltage);
    Serial.print("Battery Percentage: ");
    Serial.println(batteryPercentage);
}

// Function to update RTC from NTP and handle synchronization failures
bool setRTCFromNTP() {
    Serial.println("Attempting to sync time with NTP...");
    configTime(0, 0, ntpServer);  // Sync time without timezone adjustments
    delay(2000);  // Wait for NTP sync
    time_t nowTime = time(nullptr);
    int retryCount = 0;

    // Retry loop for fetching time, up to 15 attempts
    while (nowTime < 1609459200 && retryCount < 15) {  // Wait until time is valid (> 01/01/2021)
        delay(1000);
        nowTime = time(nullptr);  // Get the current time
        retryCount++;
        Serial.print("Waiting for NTP time... Attempt: ");
        Serial.println(retryCount);
    }

    if (nowTime >= 1609459200) {
        lastRefreshTimestamp = nowTime;
        Serial.println("NTP Time Sync Successful.");
        Serial.print("Raw time from NTP (UTC): ");
        Serial.println(nowTime);

        // Display the UTC time
        struct tm timeinfo;
        gmtime_r(&nowTime, &timeinfo);
        char buffer[64];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S UTC", &timeinfo);
        Serial.print("UTC time after NTP sync: ");
        Serial.println(buffer);

        return true;  // Time successfully fetched
    }

    Serial.println("NTP Time Sync Failed.");
    return false;  // Time sync failed after retries
}

// Use RTC time as fallback when NTP sync fails
void fallbackToRTCTime() {
    time_t rtcTime = time(NULL);  // Use the RTC's time
    if (rtcTime < 1609459200) {  // Check if RTC time is also invalid
        Serial.println("RTC Time Invalid.");
    } else {
        Serial.println("Using RTC time as fallback.");
        lastRefreshTimestamp = rtcTime;  // Set the last refresh to the current RTC time

        Serial.print("RTC time_t value: ");
        Serial.println(rtcTime);

        // Display the UTC time
        struct tm timeinfo;
        gmtime_r(&rtcTime, &timeinfo);
        char buffer[64];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S UTC", &timeinfo);
        Serial.print("UTC RTC time: ");
        Serial.println(buffer);
    }
}

// WiFi reconnection
bool reconnectWiFi() {
    Serial.println("Attempting to connect to WiFi...");
    WiFi.begin(ssid, password);
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 10) {
        delay(1000);
        retries++;
        Serial.println("Connecting to WiFi...");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi Connected.");
        Serial.print("Device IP Address: ");
        Serial.println(WiFi.localIP());
        return true;
    } else {
        Serial.println("WiFi Connection Failed.");
        return false;
    }
}

// Function to parse date-time strings in local time and convert to UTC
time_t parseDateTime(String dateTimeStr) {
    struct tm tm;
    memset(&tm, 0, sizeof(struct tm));

    if (sscanf(dateTimeStr.c_str(), "%d-%d-%d %d:%d:%d",
               &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) {
        Serial.print("Failed to parse dateTime: ");
        Serial.println(dateTimeStr);
        return 0;
    }

    tm.tm_year -= 1900; // Adjust years since 1900
    tm.tm_mon -= 1;     // Adjust months 0-11

    // Time is in local time; convert to time_t
    time_t localTime = mktime(&tm);

    // Convert local time to UTC
    time_t utcTime = localTime - getLocalTimeOffset(localTime);

    // Debug prints
    Serial.println("----------");
    Serial.print("Parsed dateTime (local): ");
    Serial.println(dateTimeStr);
    Serial.print("Local time_t value: ");
    Serial.println(localTime);

    struct tm *localTm = localtime(&localTime);
    char localBuffer[64];
    strftime(localBuffer, sizeof(localBuffer), "%Y-%m-%d %H:%M:%S", localTm);
    Serial.print("Local time after parsing: ");
    Serial.println(localBuffer);

    Serial.print("UTC time_t value: ");
    Serial.println(utcTime);

    struct tm *utcTm = gmtime(&utcTime);
    char utcBuffer[64];
    strftime(utcBuffer, sizeof(utcBuffer), "%Y-%m-%d %H:%M:%S UTC", utcTm);
    Serial.print("UTC time after conversion: ");
    Serial.println(utcBuffer);
    Serial.println("----------");

    return utcTime;
}

// Fetch and parse calendar events from Home Assistant
bool fetchCalendarEvent(const char* url, String& calendarName, String& message, time_t& startTime, time_t& endTime) {
    Serial.print("Fetching event from URL: ");
    Serial.println(url);
    HTTPClient http;
    http.begin(url);
    http.addHeader("Authorization", homeAssistantToken);
    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        DynamicJsonDocument doc(4096);  // Increased size to handle event data
        DeserializationError error = deserializeJson(doc, payload);
        if (error) {
            Serial.print("deserializeJson() failed: ");
            Serial.println(error.c_str());
            return false;
        }

        // Extract the relevant fields from the response
        calendarName = doc["attributes"]["friendly_name"].as<String>();
        message = doc["attributes"]["message"].as<String>();
        String start = doc["attributes"]["start_time"].as<String>();
        String end = doc["attributes"]["end_time"].as<String>();

        Serial.println("========== Event Details ==========");
        Serial.print("Calendar Name: ");
        Serial.println(calendarName);
        Serial.print("Event Message: ");
        Serial.println(message);
        Serial.print("Start time string: ");
        Serial.println(start);
        Serial.print("End time string: ");
        Serial.println(end);

        // Parse start and end times
        startTime = parseDateTime(start);
        endTime = parseDateTime(end);

        return true;
    } else {
        Serial.print("Error fetching event. HTTP code: ");
        Serial.println(httpCode);
        return false;
    }
}

// Display the current date at the top left in blue and underline it
void displayCurrentDate() {
    display.setFont(&FreeMonoBoldOblique12pt7b);  // Set the font for the date
    display.setTextSize(2);
    display.setTextColor(INKPLATE_BLUE);  // Display the date in blue
    int dateY = 40;
    display.setCursor(10, dateY);  // Position the date at the top left
    time_t currentTime = time(NULL);

    // Check if the time is valid before printing
    if (currentTime < 1609459200) {  // If time is still invalid
        display.print("Waiting for time...");
        Serial.println("Time is not set correctly.");
    } else {
        // Convert UTC time to local time
        time_t localTime = currentTime + getLocalTimeOffset(currentTime);

        // Format and display the local date
        struct tm localTimeInfo;
        localtime_r(&localTime, &localTimeInfo);
        char dateBuffer[16];
        strftime(dateBuffer, sizeof(dateBuffer), "%m/%d/%Y", &localTimeInfo);  // Local date format MM/DD/YYYY
        display.print(dateBuffer);
        Serial.print("Current Local Date: ");
        Serial.println(dateBuffer);

        // Underline the date
        int dateWidth = display.getCursorX() - 10;  // Calculate date width for underline
        display.drawLine(10, dateY + 5, 10 + dateWidth, dateY + 5, INKPLATE_BLUE);
    }
}

// Draw the bottom 1/3rd with the chosen color and expand it 10% upward
void drawBottomThird(const char* status, uint16_t backgroundColor) {
    int screenHeight = display.height();
    int screenWidth = display.width();

    // Fill the bottom 1/3rd of the screen and expand the top by 10%
    int boxHeight = screenHeight / 3 + (screenHeight / 10);
    int boxY = screenHeight - boxHeight;

    display.fillRect(0, boxY, screenWidth, boxHeight, backgroundColor);

    // Set text color: black if background is yellow, else white
    if (backgroundColor == INKPLATE_YELLOW) {
        display.setTextColor(INKPLATE_BLACK);
    } else {
        display.setTextColor(INKPLATE_WHITE);
    }

    // Display the status message
    display.setFont(&FreeMonoBold24pt7b);
    display.setTextSize(4);
    display.setCursor(10, screenHeight - (screenHeight / 7.5));  // Lowered by 15%
    display.print(status);
}

// Display calendar events and status (FREE, BUSY, FOCUS)
void displayCalendarEvents() {
    display.clearDisplay();

    // Display the current date at the top in blue with an underline
    displayCurrentDate();

    display.setFont(&FreeMonoBoldOblique12pt7b);  // Set font for event titles
    display.setTextSize(1);  // Shrink the font size for event details
    int cursorY = 80;  // Adjusted Y position to avoid overlapping with the date
    int ongoingEventsCount = 0;
    time_t currentTime = time(NULL);  // Get the current UTC time
    Serial.print("Current time_t value (UTC): ");
    Serial.println(currentTime);

    time_t localCurrentTime = currentTime + getLocalTimeOffset(currentTime);

    struct tm localTimeInfo;
    localtime_r(&localCurrentTime, &localTimeInfo);
    char timeString[64];
    strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &localTimeInfo);
    Serial.print("Current Local Time: ");
    Serial.println(timeString);

    time_t nextEventStartTime = 0;
    int nextEventIndex = -1;

    // Determine the number of calendar elements in the array
    int numCalendars = sizeof(calendarMap) / sizeof(calendarMap[0]);

    // Loop through the calendar events
    for (int i = 0; i < numCalendars; i++) {
        String calendarName, message;
        time_t startTimeUTC, endTimeUTC;

        if (fetchCalendarEvent(calendarMap[i].url, calendarName, message, startTimeUTC, endTimeUTC)) {
            // Determine the next event
            if (startTimeUTC > currentTime && (nextEventStartTime == 0 || startTimeUTC < nextEventStartTime)) {
                nextEventStartTime = startTimeUTC;
                nextEventIndex = i;
            }

            // Set the text color based on whether this is the next event or not
            if (i == nextEventIndex) {
                display.setTextColor(INKPLATE_GREEN);  // Next event in green
            } else {
                display.setTextColor(INKPLATE_BLACK);  // Other events in black
            }

            // Display the event
            display.setCursor(10, cursorY);
            display.print(String(calendarMap[i].name) + ": " + message);
            cursorY += 20;

            // Convert event times to local time for display
            time_t startTimeLocal = startTimeUTC + getLocalTimeOffset(startTimeUTC);
            time_t endTimeLocal = endTimeUTC + getLocalTimeOffset(endTimeUTC);

            // Display event times
            display.setCursor(10, cursorY);
            display.print(formatTime(startTimeLocal) + " - " + formatTime(endTimeLocal));
            cursorY += 30;

            // Debug event times
            Serial.println("----- Event Time Details -----");
            Serial.print("Event Name: ");
            Serial.println(message);

            // Start Time
            Serial.print("Start Time (Local): ");
            struct tm startTimeInfo;
            localtime_r(&startTimeLocal, &startTimeInfo);
            char startBuffer[64];
            strftime(startBuffer, sizeof(startBuffer), "%Y-%m-%d %H:%M:%S", &startTimeInfo);
            Serial.println(startBuffer);

            Serial.print("Start Time (UTC): ");
            struct tm startTimeUtcInfo;
            gmtime_r(&startTimeUTC, &startTimeUtcInfo);
            char startUtcBuffer[64];
            strftime(startUtcBuffer, sizeof(startUtcBuffer), "%Y-%m-%d %H:%M:%S UTC", &startTimeUtcInfo);
            Serial.println(startUtcBuffer);

            // End Time
            Serial.print("End Time (Local): ");
            struct tm endTimeInfo;
            localtime_r(&endTimeLocal, &endTimeInfo);
            char endBuffer[64];
            strftime(endBuffer, sizeof(endBuffer), "%Y-%m-%d %H:%M:%S", &endTimeInfo);
            Serial.println(endBuffer);

            Serial.print("End Time (UTC): ");
            struct tm endTimeUtcInfo;
            gmtime_r(&endTimeUTC, &endTimeUtcInfo);
            char endUtcBuffer[64];
            strftime(endUtcBuffer, sizeof(endUtcBuffer), "%Y-%m-%d %H:%M:%S UTC", &endTimeUtcInfo);
            Serial.println(endUtcBuffer);

            // Check if the event is ongoing
            if (currentTime >= startTimeUTC && currentTime < endTimeUTC) {
                ongoingEventsCount++;
                Serial.println("Status: Event is ongoing.");
            } else {
                Serial.println("Status: Event is not ongoing.");
            }
            Serial.println("-------------------------------");
        }
    }

    // Determine status based on the number of ongoing events
    if (ongoingEventsCount == 0) {
        drawBottomThird("FREE", INKPLATE_GREEN);  // No events, green background with white "FREE" text
        Serial.println("Current Status: FREE");
    } else if (ongoingEventsCount == 1) {
        drawBottomThird("BUSY", INKPLATE_YELLOW);  // One event, yellow background with black "BUSY" text
        Serial.println("Current Status: BUSY");
    } else {
        drawBottomThird("FOCUS", INKPLATE_RED);  // Two or more events, red background with white "FOCUS" text
        Serial.println("Current Status: FOCUS");
    }

    // Display the last refresh time in white text at the very bottom
    display.setFont(NULL);
    display.setTextSize(1);
    display.setTextColor(INKPLATE_WHITE);
    display.setCursor(10, display.height() - 20);
    time_t lastRefreshLocal = lastRefreshTimestamp + getLocalTimeOffset(lastRefreshTimestamp);
    display.print("Last Refresh: " + formatTime(lastRefreshLocal));

    // Debug last refresh time
    Serial.print("Last Refresh Time_t (UTC): ");
    Serial.println(lastRefreshTimestamp);
    struct tm refreshTimeInfo;
    localtime_r(&lastRefreshLocal, &refreshTimeInfo);
    char refreshBuffer[64];
    strftime(refreshBuffer, sizeof(refreshBuffer), "%Y-%m-%d %H:%M:%S", &refreshTimeInfo);
    Serial.print("Last Refresh Local Time: ");
    Serial.println(refreshBuffer);

    // Display the battery status
    displayBatteryStatus();

    display.display();
}

// Format time to HH:MM AM/PM for display
String formatTime(time_t timeVal) {
    struct tm timeInfo;
    localtime_r(&timeVal, &timeInfo);
    char buf[16];
    strftime(buf, sizeof(buf), "%I:%M %p", &timeInfo);  // 12-hour format with AM/PM
    return String(buf);
}

// Setup function
void setup() {
    Serial.begin(115200);  // Initialize Serial Monitor
    display.begin();

    // No time zone settings needed, times are in UTC

    if (!reconnectWiFi()) {
        display.clearDisplay();
        display.setCursor(10, 50);
        display.setFont(&FreeMonoBoldOblique12pt7b);
        display.print("WiFi Error");
        display.display();
        fallbackToRTCTime();  // Use RTC time if WiFi fails
    } else {
        if (!setRTCFromNTP()) {
            display.clearDisplay();
            display.setCursor(10, 50);
            display.setFont(&FreeMonoBoldOblique12pt7b);
            display.print("NTP Time Sync Failed");
            display.display();
            fallbackToRTCTime();  // Use RTC time if NTP fails
        }
    }

    displayCalendarEvents();

    // Sleep for 15 minutes
    esp_sleep_enable_timer_wakeup(15 * 60 * 1000000);  // 15 minutes
    esp_deep_sleep_start();
}

// Empty loop (device sleeps)
void loop() {}