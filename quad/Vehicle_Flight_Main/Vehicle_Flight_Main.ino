//====================================================================================================
//                                     Vehicle_Flight_Code                                              // WHAT ABOUT DELAY TIMES BOTH ON THE TRANSMITTING END AND THE RECEIVING END 
//====================================================================================================

#include <Adafruit_GPS.h>                         // used by: GPS
#include <math.h>                                 // used by: GPS
#include <XBee.h>                                 // used to receive XBee communication from transponder
#include <Wire.h>                                 // used by BMP180 : Barometer + BN0055
#include <Adafruit_Sensor.h>                      // used by BMP180 : Barometer + BN0055   
#include <Adafruit_BMP085_U.h>                    // used by BMP180 : Barometer
#include <Adafruit_BNO055.h>                      // used by BN0055
#include <utility/imumaths.h>                     // used by BN0055
#include <PID_v1.h>                               // used for control loops.
#include <Servo.h>                                // Used for PWM outputs to Pixhawk Controller

//====================================================================================================
//                                       Creating Objects                                           
//====================================================================================================
                                                                           
#define usbSerial Serial                                                                            // Serial Communication.
#define gpsSerial Serial1
#define xbeeSerial Serial2

Adafruit_BMP085_Unified bmp = Adafruit_BMP085_Unified(10085);                                       // BMP180 Barometer.
Adafruit_BNO055 bno = Adafruit_BNO055();                                                            // BN0055 Accelerometer.
Adafruit_GPS GPS(&gpsSerial);                                                                       // GPS Hardware.

#define GPSECHO false

//====================================================================================================
//                               General_Global_Variables/Declarations                                           
//====================================================================================================

// GPS Variables.
SIGNAL(TIMER0_COMPA_vect)                                 // Interrupt is called once a millisecond, looks for any new GPS data, and stores it.
{
  GPS.read();
}
float currentLat,currentLong;
String latConversion,longConversion;
float targetLat,targetLong;
int targetHeading;
boolean usingInterrupt = false;
void useInterrupt(boolean);                               // Function prototype keeps arduino happy?

// XBee Parsing Variables.
int comma = 0;
int newLine = 0;
String inputString = "";
boolean stringComplete = false;

// Vehicle Flight Data.
double distanceToTarget;
float groundPressure; 

// PWM Servo Outputs.
Servo throttle;
Servo pitch;
Servo roll;
Servo yaw;
        
// Timer Variables.
unsigned long start = millis();
uint32_t usbPrintTimer, xbeePrintTimer;

// Testing
int testAlt = 5;                                            // Analog read pin.

//====================================================================================================
//                                     PID_Variables                                          
//====================================================================================================

// Altitude 
double setAltitude = 50;                                      // Around 150 feet?
double currentAltitude;                                       // Comes from the barometer data
double altError;                                              // PID has to use type: double?
double throttleOut;                                           // Pwm Output...Convert to ppm using board.
double AltAggKp=4, AltAggKi=0.2, AltAggKd=1;                  // Define Aggressive and Conservative Tuning Parameters.
double AltConsKp=1, AltConsKi=0, AltConsKd=0.25;              // Not sure how to decide startting values.
PID altPID (&currentAltitude, &throttleOut, &setAltitude,     // Specify the links and initial tuning parameters
            AltConsKp, AltConsKi, AltConsKd, DIRECT);
  
// Yaw
double yawOut;
double currentHeading;
double desiredHeading;                                        // 0 Degree N at all times.
double yawConsKp=1, yawConsKi=0, yawConsKd=0;                 // Not sure how to decide startting values.
PID yawPID(&currentHeading, &yawOut, &desiredHeading,
           yawConsKp,yawConsKi,yawConsKd,DIRECT);
           
// Roll
double rollOut;
double currentRoll;
double desiredRoll;                                           // Calculated as a function of Ed and Eh.
double rollConsKp=1, rollConsKi=0, rollConsKd=0;
PID rollPID(&currentRoll,&rollOut,&desiredRoll,
            rollConsKp,rollConsKi,rollConsKd,DIRECT);
            
// Pitch
double pitchOut;
double currentPitch;
double desiredPitch;
double pitchConsKp=1, pitchConsKi=0, pitchConsKd=0;
PID pitchPID(&currentPitch,&pitchOut,&desiredPitch,
            pitchConsKp,pitchConsKi,pitchConsKd,DIRECT);

//====================================================================================================
//                                             SETUP
//====================================================================================================

void setup(void)
{
  // Start GPS and set desired configuration
  GPS.begin(9600);                                            // 9600 NMEA default speed
  gpsSerial.begin(9600);
  GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);               // turns on RMC and GGA (fix data)
  GPS.sendCommand(PMTK_SET_NMEA_UPDATE_1HZ);                  // 1 Hz update rate suggested...
  GPS.sendCommand(PGCMD_NOANTENNA);                           // turn off antenna status info
  useInterrupt(true);                                         // use interrupt to constantly pull data from GPS
  delay(1000);
  
  // XBEE 
  xbeeSerial.begin(57600);
  inputString.reserve(30);                                    // reserve 30 bytes for the inputString:

  // BMP180 Barometer.
  if(!bmp.begin())
  {
    /* There was a problem detecting the BMP085 ... check your connections */
    usbSerial.print("Ooops, no BMP085 detected ... Check your wiring or I2C ADDR!");
    while(1);
  }
  
  groundPressure = getGroundPressure();                      // Sets the ground level pressure.

  // BN0055 Accelerometer.
  if(!bno.begin())
  {
    /* There was a problem detecting the BNO055 ... check your connections */
    usbSerial.print("Ooops, no BNO055 detected ... Check your wiring or I2C ADDR!");
    while(1);
  }
  
  delay(1000);
  bno.setExtCrystalUse(true);
  
  // USB Serial Port
  usbSerial.begin(115200);
  usbSerial.println("Adafruit GPS library basic test!");
   
  // altPID
  throttle.attach(13);
  processBaro(groundPressure);
  altError = currentAltitude - setAltitude;
  altPID.SetMode(AUTOMATIC);
  altPID.SetOutputLimits(1000,2000);

  // yawPID
  yaw.attach(12);
  altPID.SetMode(AUTOMATIC);
  yawPID.SetOutputLimits(1000,2000);

  // rollPID
  roll.attach(11);
  altPID.SetMode(AUTOMATIC);
  rollPID.SetOutputLimits(1000,2000);

  // pitchPID
  pitch.attach(10);
  pitchPID.SetMode(AUTOMATIC);
  pitchPID.SetOutputLimits(1000,2000);

  // Safety
  //checkGPSfix();                                            // Loop until it has a fix.
  //checkXbeeFix();                                           // Loop until we have parsed a non-zero value from transponder.
  altCheck();                                                 // Loop until we have good altimeter calibration.

  // Testing
  pinMode(testAlt,INPUT);
}

//====================================================================================================
//                                             Main LOOP
//====================================================================================================

void loop(void)
{
  // Process GPS 
   if (GPS.newNMEAreceived())                                 // check for updated GPS information
   {                                      
     if(GPS.parse(GPS.lastNMEA()) )                           // if we successfully parse it, update our data fields
     {
      processGPS();
     }
   }
  // Gather Data 
  processXbee();
  processBaro(groundPressure);                                // Get current altitude.
  processIMUheading();
  
  // Calculate.
  distanceToWaypoint();
  courseToWaypoint();
  
//  // PID Control.
  altPIDloop();
  yawPIDloop();
  rollPIDloop();
  pitchPIDloop();
  
  // Print (Debugging).
  printTarget();                                             // Print transponder data.
  printScreen();                                             // Vehicle GPS Data.

  // Test mode Alt.
//  while(testAlt)
//{
//  testAltPID();
//}
   
}

// INFINTIE LOOP TO PREVENT FLYING WHEN THERE IS A LOSS IN COMMUNICATION? 
// OR AUTOMATIC SWITCH AWAY FROM AUTONMOUS CONTROL ALGORITHM?  
// ERROR CHECK FOR FLIGHT FAILURE.  
//***************************************************************************************************************************************************************
//***************************************************************************************************************************************************************
