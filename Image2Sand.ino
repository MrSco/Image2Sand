/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
INCLUDED LIBRARIES
*/
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


#include <Arduino.h>
#include <elapsedMillis.h>            //Creates timer objects that are more convenient for non-blocking timing than millis()
#include <AccelStepper.h>             //Controls the stepper motors
#include <FastLED.h>                  //Controls the RGB LEDs
#include <OneButtonTiny.h>            //Button management and debouncing


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
PREPROCESSOR DIRECTIVES.

Useful values and limits for defining how the sand garden will behave. In most cases, these values should not be changed.

*/
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


// #define REVERSE_R_MOTOR               //uncomment this line to switch the direction the radial axis motor runs 
// #define REVERSE_A_MOTOR               //uncomment this line to switch the direction the angular axis motor runs

#define STEPS_PER_MOTOR_REV   2048                                    //Number of motor steps in one revolution of the output shaft of the motor. 
#define STEPS_PER_A_AXIS_REV  2 * STEPS_PER_MOTOR_REV                 //the number of steps required to move the angular axis one full revolution
#define TRAVEL_PER_PINION_REV 50.267                                  //Distance in mm the rack moves in one complete revolution of the pinion.
#define STEPS_PER_MM          81.4849                                 //Precomputed motor steps per mm of radial axis travel. 
#define MM_PER_STEP           1.0 / STEPS_PER_MM                      //Millimeters of travel per motor step on radial axis. Evaluates to 0.01227 if STEPS_PER_REV is 2048. 
#define STEPS_PER_DEG         (STEPS_PER_A_AXIS_REV) / 360            //Motor steps per degree of motion on angular axis. Should be about 11.378 steps per degree.
#define DEG_PER_STEP          1 / STEPS_PER_DEG                       //Degrees of rotation on angular axis per motor step. About .08799 degrees.
#define STEPS_PER_RAD         STEPS_PER_MOTOR_REV / PI                //Motor steps per radian of motion on angular axis. About 652. 
#define RAD_PER_STEP          1 / STEPS_PER_RAD                       //Radians travelled on angular axis per motor step. About 0.00153

#define ACTUAL_LEN_R_MM       87.967                                  //Length in mm of the radial axis (hard limits). Derived from the CAD model of the hardware.
#define ACTUAL_LEN_R_STEPS    ACTUAL_LEN_R_MM * STEPS_PER_MM          //Maximum possible length of radius in steps of motor (hard limits). Should be 7167 when 2048 steps per rev in motor.
#define MAX_R_STEPS           7000                                    //Soft limit on how far the radius can move in terms of steps of the motor. This leaves a slight buffer on each end.
#define MAX_R_MM              MAX_R_STEPS * MM_PER_STEP               //Soft limit length in mm of the radial axis. 85.91mm. 

#define HOMING_BUFFER         (ACTUAL_LEN_R_STEPS - MAX_R_STEPS) / 2  //Crash home R axis to 0, then move this many steps in positive direction to create a soft stop.
#define RELAXATION_BUFFER     STEPS_PER_DEG * 5                       //Crash homing tensions the bead chain, and backlash and flex in the gantry need to be released.

#define MAX_SPEED_R_MOTOR     550.0                                   //Maximum speed in steps per second for radius motor. Faster than this is unreliable.
#define MAX_SPEED_A_MOTOR     550.0                                   //Maximum speed in steps per second for angle motor.


//The following is used to reduce angular speed linearly with the current position on the radial axis.
//This helps the ball move at a more consistent speed through the sand regardless of how far out it is on the radial axis.
//This is just a linear function that can be fine tuned by changing the amount removed from the max speed (currently 50.0).
//Essentially what this does is drops the speed of the angular axis to 50.0 steps per second at the very outer edge of 
//the actual length of the radial axis. This point is unreachable in typical use because of the soft limits.
#define ANGULAR_SPEED_SCALAR  (MAX_SPEED_A_MOTOR  - 150.0) / (MAX_R_STEPS)    

//Pin definitions follow. 
//The #ifdef / #endif blocks are used to check to see if either REVERSE_R_MOTOR or REVERSE_A_MOTOR
//is defined at the very top of the code, and if they are, the order the pins are defined in changes.

#ifdef REVERSE_A_MOTOR
  #define MOTORA_IN1_PIN   12
  #define MOTORA_IN2_PIN   11
  #define MOTORA_IN3_PIN   10
  #define MOTORA_IN4_PIN   9
#endif

#ifndef REVERSE_A_MOTOR
  #define MOTORA_IN1_PIN   9
  #define MOTORA_IN2_PIN   10
  #define MOTORA_IN3_PIN   11
  #define MOTORA_IN4_PIN   12
#endif

#ifdef REVERSE_R_MOTOR
  #define MOTORR_IN1_PIN   5         
  #define MOTORR_IN2_PIN   6         
  #define MOTORR_IN3_PIN   7
  #define MOTORR_IN4_PIN   8
#endif

#ifndef REVERSE_R_MOTOR
  #define MOTORR_IN1_PIN   8         //The motor is flipped upside down in assembly, so pin order is reversed from other motor.
  #define MOTORR_IN2_PIN   7         
  #define MOTORR_IN3_PIN   6
  #define MOTORR_IN4_PIN   5
#endif

  
#define JOYSTICK_A_PIN   A2          //Left-right axis of joystick, associated with changing angular axis in manual mode
#define JOYSTICK_R_PIN   A3          //Up-down axis of joystick, associated with changing radial axis in manual mode
#define BUTTON_PIN       A1          //Joystick button pin
#define RANDOM_SEED_PIN  A6          //used to generate random numbers.
#define LED_DATA_PIN     A0          //The output for the LED bar.
#define NUM_LEDS         8           //Number of LEDs in the bar.
#define MAX_BRIGHTNESS   40          //Brightness values are 8-bit for a max of 255 (the range is [0-255]), this sets default maximum to 40 out of 255.
#define LED_FADE_PERIOD  1000        //Amount of time in milliseconds it takes for LEDs to fade on and off.


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
MOTION CONTROL AND PATTERN GENERATION

The following items are for tracking the position of the gantry, reading the joystick, and defining target positions for 
the gantry through the use of pattern functions. 
*/
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

//Create two objects, one for each stepper motor.
AccelStepper stepperAngle(4, MOTORA_IN1_PIN, MOTORA_IN3_PIN, MOTORA_IN2_PIN, MOTORA_IN4_PIN);     //angular axis motor
AccelStepper stepperRadius(4, MOTORR_IN1_PIN, MOTORR_IN3_PIN, MOTORR_IN2_PIN, MOTORR_IN4_PIN);    //radial axis motor

//Struct used for storing positions of the axes, as well as storing the values of the joystick.
struct Positions {
  short int radial;                     // the units for these values are motor steps; in DrawPicture the units are scaled from 0-1000
  short int angular;                    // in DrawPicture the units are degrees * 10
};                                

//These variables of type Positions (defined above) are for storing gantry positions and joystick values
Positions currentPositions;       //store the current positions of the axes in this
Positions targetPositions;        //store the desired positions of the axes in this
Positions joystickValues;         //store the potentiometer values of the joystick in this. I didn't want to make a new struct just for the joystick.

//Function prototypes for pattern generators. Each pattern function has to return a struct of type Positions. 
//This will be used as the target position for the motion controller. Note that these are just
//function prototypes. They are put up here to let the compiler know that they will be defined later in the code.
Positions pattern_SimpleSpiral(Positions current, bool restartPattern = false);               //Simple spiral. Grows outward, then inward.
Positions pattern_Cardioids(Positions current, bool restartPattern = false);                  //Cardioids
Positions pattern_WavySpiral(Positions current, bool restartPattern = false);                 //Wavy spiral.
Positions pattern_RotatingSquares(Positions current, bool restartPattern = false);            //Rotating squares
Positions pattern_PentagonSpiral(Positions current, bool restartPattern = false);             //Pentagon spiral
Positions pattern_HexagonVortex(Positions current, bool restartPattern = false);              //Hexagon vortex
Positions pattern_PentagonRainbow(Positions current, bool restartPattern = false);            //Pentagon rainbow
Positions pattern_RandomWalk1(Positions current, bool restartPattern = false);                //Random walk 1 (connected by arcs)
Positions pattern_RandomWalk2(Positions current, bool restartPattern = false);                //Random walk 2 (connected by lines)
Positions pattern_AccidentalButterfly(Positions current, bool restartPattern = false);        //Accidental Butterfly
Positions pattern_Picture(Positions current, bool restartPattern = false);                    //Custom Picture

/**
 * @brief Typedef for storing pointers to pattern-generating functions.
 * 
 * This typedef defines a custom data type PatternFunction for storing pointers to pattern functions. 
 * It allows pattern functions to be called by passing the appropriate index number to an array of pattern function pointers, 
 * simplifying the process of switching between patterns. Each pattern function takes a Positions struct and a bool as parameters 
 * and returns the next target position as a Positions struct.
 * 
 * @typedef PatternFunction
 * 
 * This typedef enables pattern switching by indexing into an array of pattern functions, making it easy to select and execute 
 * different patterns dynamically.
 */
typedef Positions (*PatternFunction)(Positions, bool);

/**
 * @brief Array of pattern-generating functions.
 * 
 * This array stores the functions responsible for generating different patterns, defined using the PatternFunction typedef. 
 * To add a new pattern function, follow these steps:
 * 1. Declare the new pattern function prototype (e.g., Positions pattern_42(Positions current);).
 * 2. Add the new pattern function to this array.
 * 3. Define the function at the end of the code.
 * 
 * @note The array is 0-indexed, but the controller interface (joystick and LEDs) uses 1-indexing. 
 * Therefore, pattern 1 is stored at index 0, pattern 2 at index 1, and so on. This offset is handled within the code, 
 * but keep it in mind when working with the array.
 */
PatternFunction patterns[] = {pattern_SimpleSpiral, pattern_Cardioids, pattern_WavySpiral, pattern_RotatingSquares, pattern_PentagonSpiral, pattern_HexagonVortex, pattern_PentagonRainbow, pattern_RandomWalk1,
                              pattern_RandomWalk2, pattern_AccidentalButterfly, pattern_Picture};


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
STATE MACHINE FLAGS:
This code uses simple state machine to keep track of which mode the machine is in (e.g., actively running a pattern, or in pattern selection mode).
These flags are used in that state machine.
*/
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

int currentPattern = 1;           //default to pattern 1.
bool runPattern = false;          //this will be the start/stop flag. true means run the selected pattern.
bool buttonShortPressed = false;  //button pressed state flag.
bool buttonLongPressed = false;   //for indicating long press
bool autoMode = true;             //tracking if we're in automatic or manual mode. Defaults to auto on startup. If you want to start in manual drawing mode, set this to false.
bool motorsEnabled = true;        //used to track if motor drivers are enabled/disabled. initializes to enabled so the homing sequence can run.
bool patternSwitched = false;     //used for properly starting patterns from beginning when a new pattern is selected
int lastPattern = currentPattern; //used with currentPattern to detect pattern switching and set the patternSwitched flag.


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
FUNCTION PROTOTYPES.

These are basically a way of telling the compiler that we will have functions with these names and parameters, which will be defined later in the code. 
*/
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

//Math related functions
long convertDegreesToSteps(float degrees);                                      //for converting degrees to motor steps
float convertStepsToDegrees(int steps);                                         //for converting motor steps to degrees
long convertRadiansToSteps(float rads);                                         //For converting radians to steps on angular axis
float convertStepsToRadians(float steps);                                       //For converting steps to radians on angular axis
int convertMMToSteps(float mm);                                                 //for converting millimeters to steps on radial axis
float convertStepsToMM(float steps);                                            //for converting steps to millimeters on the radial axis
float fmap(float n, float in_min, float in_max, float out_min, float out_max);  //version of map() that works for floating point numbers
int modulus(int x, int y);                                                      //Use for wrapping values around at ends of range. like %, but no negative numbers.

//Movement related functions
int findShortestPathToPosition(int current, int target, int wrapValue);         //For finding the shortest path to the new position on the angular axis
int calcRadialChange(int angularMoveInSteps, int radialMoveInSteps);            //for figuring out the relative change on the radial axis
int calcRadialSteps(int current, int target, int angularOffsetSteps);           //For calculating actual number of steps radial motor needs to take.
int calculateDistanceBetweenPoints(Positions p1, Positions p2);                 //calculate distance between two points in polar coordinates. Not currently used, but useful
void homeRadius();                                                              //for homing the radial axis on startup
void moveToPosition(long angularSteps, long radialSteps);                       //for moving both axes to target position simultaneously.
Positions orchestrateMotion(Positions currentPositions, Positions targetPositions);                      //Encapsulates steps required to move to target position and returns the new current position.

//Miscellaneous functions
Positions readJoystick(void);                                                   //returns a struct containing the current joystick values

//Geometry generation functions
Positions drawLine(Positions point0, Positions point1, Positions current, int resolution, bool reset);             //For drawing a straight line between two points
void nGonGenerator(Positions *pointArray, int numPoints, Positions centerPoint, int radius, float rotationDeg = 0.0);   //generates a list of points that form a polygon's vertices
void translatePoints (Positions *pointArray, int numPoints, Positions translationVector);              //For moving an array of points along a vector to a new position
void scalePoints (Positions *pointArray, int numPoints, float scaleFactor);                            //NOT IMPLEMENTED - For scaling a shape represented by a point array up or down in size
void rotatePoints (Positions *pointArray, int numPoints, Positions rotationCenter, float rotationDeg); //NOT IMPLEMENTED - For rotating a point array around an arbitrary point
void reflectPoints(Positions *pointArray, int numPoints, Positions reflectionVector);                  //NOT IMPLEMENTED - For reflecting a point array across an arbitrary line


#pragma region LedDisplayClass

/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
This is a class that contains all the functions and data required to handle the LED display bar.
*/
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class LedDisplay {
private:
  CRGB leds[NUM_LEDS];        //array that holds the state of each LED

public:
  //This is the constructor. It's called when a new instance of the class is created, and handles setting things up for use.
  LedDisplay() {              
    FastLED.addLeds<WS2812B, LED_DATA_PIN, GRB>(leds, NUM_LEDS);
    FastLED.setBrightness(MAX_BRIGHTNESS);
    FastLED.clear();
  }

  //a proxy function for setting the brightness of the LEDs. This way the class can handle all the LED stuff
  //without relying on the user to sometimes call on FastLED directly.
  void setBrightness(uint8_t val) {
    FastLED.setBrightness(val);
  }

/**
 * @brief Indicates the currently selected pattern by lighting up LEDs in a specific color.
 *
 * This function uses the FastLED library to light up the LEDs in a specific pattern to indicate which pattern is selected. 
 * A solid color is shown to indicate that the machine is in pattern selection mode. If the value is 255, the manual drawing 
 * mode is indicated by lighting a single LED in DarkCyan. For other pattern values, the LEDs are lit using bitwise operations 
 * to determine which LEDs should be turned on.
 *
 * @param value The current pattern number, where 255 indicates manual drawing mode, and other values indicate specific patterns.
 * 
 * @note The .fadePixels() method can be used to make the LEDs fade, indicating that the machine is running a pattern. This function 
 * uses bitwise operations to determine the LED pattern, lighting the LEDs in MediumVioletRed for non-manual patterns.
 */
  void indicatePattern(uint8_t value) {                     //used for showing which pattern is selected
    FastLED.clear();
    if (value == 255) {                                     //pattern255 is the manual drawing mode.
      FastLED.clear();
      leds[0] = CRGB::DarkCyan;
    } else {                                                //all other patterns can be displayed with bitwise operations
      for (int i = 0; i < NUM_LEDS; i++) {                  //iterate through each LED in the array
        if (value & (1 << i)) {                             //bitwise AND the value of each bit in the pattern number to determine if current LED needs to be turned on. 
          leds[NUM_LEDS - 1 - i] = CRGB::MediumVioletRed;   //turn on the LED if needed
        }
      }
    }
    FastLED.show();                                         //display the LEDs
  }

  /**
 * @brief Gradually fades the LEDs on and off over time to indicate that a pattern is running.
 *
 * This function automatically controls the brightness of the LEDs, causing them to fade in and out over a specified period. 
 * It is intended to be used when the machine is running a pattern to provide a visual indication of operation.
 *
 * @param period The time in milliseconds it takes for the LEDs to fade in and out (complete cycle).
 * @param maxBrightness The maximum brightness level the LEDs will reach during the fade cycle.
 *
 * The function calculates the current brightness based on the time position in the fade cycle, applying the appropriate brightness 
 * to all LEDs using the FastLED.setBrightness() function.
 */
  void fadePixels(unsigned long period, uint8_t maxBrightness) {
    unsigned long currentTime = millis();
    unsigned long timeInCycle = currentTime % period; // Time position in current cycle
    unsigned long halfPeriod = period / 2;
    int brightness;

    // Determine phase and calculate brightness
    if (timeInCycle < halfPeriod) {
      // Fading in
      brightness = map(timeInCycle, 0, halfPeriod, 0, maxBrightness);
    } else {
      // Fading out
      brightness = map(timeInCycle, halfPeriod, period, maxBrightness, 0);
    }

    // Apply calculated brightness to all LEDs
    FastLED.setBrightness(brightness);
    FastLED.show();
  }


/**
 * @brief Animates an LED bouncing pattern during the homing process and flashes green when homing is complete.
 *
 * This function animates a bouncing light pattern on the LEDs to indicate that the gantry is in the process of homing. 
 * Once homing is complete, the LEDs flash green to signal completion. The function can block execution briefly during the 
 * flashing portion after homing is done.
 *
 * @param homingComplete A boolean flag indicating whether the homing process is complete. If set to false, the animation continues. 
 * If set to true, the LEDs flash green to indicate completion.
 *
 * The animation consists of a bouncing light pattern with a color that changes over time. When the gantry finishes homing, 
 * the LEDs flash green in a blocking manner for a brief period.
 */
  void homingSequence(bool homingComplete = false) {
    static unsigned long lastUpdate = 0;

    const byte fadeAmount = 150;
    const int ballWidth = 2;
    const int deltaHue  = 4;

    static byte hue = HUE_RED;
    static int direction = 1;
    static int position = 0;
    static int multiplier = 1;

    FastLED.setBrightness(MAX_BRIGHTNESS);

    if (!homingComplete) {                      //If the homing sequence is not complete, animate this pattern.
      if (millis() - lastUpdate >= 100) {
        hue += deltaHue;
        position += direction;

        if (position == (NUM_LEDS - ballWidth) || position == 0) direction *= -1;

        for (int i = 0; i < ballWidth; i++) {
          leds[position + i].setHue(hue);
        }

        // Randomly fade the LEDs
        for (int j = 0; j < NUM_LEDS; j++) {
          //if (random(10) > 3)
          leds[j] = leds[j].fadeToBlackBy(fadeAmount);  
        }
        FastLED.show();
        lastUpdate = millis();
      }
    } else {                                    //if the homing sequence is complete, indicate that by flashing the LEDs briefly.
      for (int i = 0; i < NUM_LEDS; i++) {
        leds[i] = CRGB::Green;
      }
      
      for (int j = 0; j < 8; j++) {
        FastLED.setBrightness(constrain(MAX_BRIGHTNESS * multiplier, 0, MAX_BRIGHTNESS));
        multiplier *= -1;
        FastLED.show();
        delay(100);
      }
    }
  }
};

#pragma endregion LedDisplayClass

//Create an instance of the LedDisplay class that controls the RGB LEDs.
LedDisplay display;


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
MISC. GLOBAL VARIABLES.
Used for tracking time and button presses.
*/
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

elapsedMillis lastJoystickUpdate;                    //used to track the last time the joystick was updated to prevent absurdly fast scrolling

//Create an object that handles the joystick button
OneButtonTiny button(BUTTON_PIN, true, true);        //set up the button (button pin, active low, enable internal pull-up resistor)


/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
SETUP FUNCTION (runs once when Arduino powers on)
*/
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void setup() {
  //Generate a random seed. If you want to use pseudorandom numbers in a pattern, this makes them more random.
  //Make sure that RANDOM_SEED_PIN is an analog pin that's not connected to anything.
  randomSeed(analogRead(RANDOM_SEED_PIN));

  //configure the joystick and button pins
  pinMode(JOYSTICK_A_PIN, INPUT);
  pinMode(JOYSTICK_R_PIN, INPUT);

  //Set up the button.
  //Single press of button is for starting or stopping the current pattern.
  button.attachClick([]() {       //This is called a lambda function. Basically it's a nameless function that runs when the button is single pressed.
    runPattern = !runPattern;     //this flips the boolean state of the variable. If it's true, this sets to false, if false set to true.
  });

  //Attaching an event to the long press of the button. Currently, long pressing the button lets you end the homing process early.
  button.attachLongPressStart([]() {
    buttonLongPressed = true;         
  });

  //set the maximum speeds and accelerations for the stepper motors.
  stepperAngle.setMaxSpeed(MAX_SPEED_A_MOTOR);
  stepperAngle.setAcceleration(5000.0);           // Need high acceleration without losing steps. 
  stepperRadius.setMaxSpeed(MAX_SPEED_R_MOTOR);
  stepperRadius.setAcceleration(5000.0);

  FastLED.clear();            //clear the LEDs
  FastLED.show();

  homeRadius();               //crash home the radial axis. This is a blocking function.
}



/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
MAIN LOOP (runs endlessly).
This manages the state machine, tracks the position of the gantry, and acquires the target positions for
the gantry from the selected pattern functions or from manual mode.
*/
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void loop() {
  //Check to see if the button has been pressed. This has to be called as often as possible to catch button presses.
  button.tick();

  //if the runPattern flag is set to true, we need to start updating target positions for the controller. run the appropriate pattern!
  if (runPattern) {
    #pragma region Running
    //make sure the motors are enabled, since we want to move them
    if (!motorsEnabled) {
      stepperAngle.enableOutputs();     //enable the motor
      stepperRadius.enableOutputs();
      motorsEnabled = true;             //set the state machine flag
    }

    //First we'll check the state machine flags to see if we're in manual drawing mode.
    if (!autoMode) {          //Not in autoMode, which means we're in manual drawing mode
      #pragma region ManualMode

      //Use the LEDs to indicate that we are in manual drawing mode
      display.setBrightness(MAX_BRIGHTNESS);
      display.indicatePattern(255);             //pattern 255 is used to indicate manual drawing mode on the LEDs

      joystickValues = readJoystick();          //read joystick values and store them in joystickValues struct
      //first check if an angular change is requested by joystick input
      if (joystickValues.angular < 0) {
        targetPositions.angular = currentPositions.angular - (STEPS_PER_MOTOR_REV / 100);    //add steps to the target position
      } else if (joystickValues.angular > 0) {
        targetPositions.angular = currentPositions.angular + (STEPS_PER_MOTOR_REV / 100);
      } else {
        targetPositions.angular = currentPositions.angular;   //otherwise maintain current position
      }
      //next check if a radial change is requested by joystick input
      if (joystickValues.radial < 0) {
        targetPositions.radial = currentPositions.radial - (MAX_R_STEPS / 100);
      } else if (joystickValues.radial > 0) {
        targetPositions.radial = currentPositions.radial + (MAX_R_STEPS / 100);
      } else {
        targetPositions.radial = currentPositions.radial;
      }

      //finally, take the steps necessary to move both axes to the target position in a coordinated manner and update the current position.
      currentPositions = orchestrateMotion(currentPositions, targetPositions);   

      #pragma endregion ManualMode
    

    //In this case, the state machine flags indicate that we're in automatic pattern mode, not manual mode.
    } else {                                        //automatic pattern mode
      #pragma region AutomaticMode
      //update the LED pattern display
      display.setBrightness(MAX_BRIGHTNESS);       
      display.indicatePattern(currentPattern);     
      
      //check to see if the pattern has been switched
      if (currentPattern != lastPattern) {
        patternSwitched = true;               //set the flag to indicate that the pattern has been changed
        lastPattern = currentPattern;         //now we can say that the last patten is the current pattern so that this if block will be false until pattern is changed again
      }

      //Call the function that will generate the pattern. 
      //This automatically calls the appropriate function from the patterns[] array.
      //Pass in the currentPositions as an argument, and the pattern function returns the targetPositions.
      //Note that the target positions are absolute coordinates: e.g., a pattern might say
      //to move to (radius, angle) = (1000 steps, 45 degrees (converted to steps)).
      //There is only one position on the sand tray that corresponds to those coordinates. 
      targetPositions = patterns[currentPattern - 1](currentPositions, patternSwitched);      //subtracing 1 here because I count patterns from 1, but the array that stores them is 0-indexed.

      patternSwitched = false;    //after we've called the pattern function above, we can reset this flag to false.

      //finally, take the steps necessary to move both axes to the target position in a coordinated manner and update the current position.
      currentPositions = orchestrateMotion(currentPositions, targetPositions);

      #pragma endregion AutomaticMode
    }
    #pragma endregion Running


  } else {    //In this case, runPattern is false, which means this is pattern selection mode
    #pragma region SelectionMode

    //if the motors are enabled, disable them to save power while they don't need to run
    if (motorsEnabled) {
      stepperAngle.disableOutputs();
      stepperRadius.disableOutputs();
      motorsEnabled = false;
    }

    //read the joystick state so that it can be used in the following if statements
    joystickValues = readJoystick();

    if (!autoMode) {                                        //This means we're not in automatic mode, so we are in manual drawing mode.
      display.indicatePattern(255);                         //The value 255 is used to represent manual mode on the LEDs.
      display.fadePixels(LED_FADE_PERIOD, MAX_BRIGHTNESS);  //update the brightness of the LEDs to fade them in and out over time

      if (lastJoystickUpdate >= 200 && (joystickValues.angular >= 90 || joystickValues.angular <= -90)) {  //the joystick is pushed all the way to the right or left
        autoMode = true;                                    //switch to automatic mode so that we can do pattern selection
        lastJoystickUpdate = 0;                             //reset the joystick update timer
      }
    } else {                                                //We're in automatic mode, which means it's time to select a pattern.
      display.indicatePattern(currentPattern);
      display.fadePixels(LED_FADE_PERIOD, MAX_BRIGHTNESS);  

      if (lastJoystickUpdate >= 200 && joystickValues.radial >= 90) {                              //if it's been 200ms since last joystick update and joystick is pushed all the way up
        currentPattern++;                                                                          //increment pattern number by 1
        if ((currentPattern == 255) || (currentPattern > sizeof(patterns)/sizeof(patterns[0]))) {  //if currentPattern equals 255 or the number of elements in the pattern array
          currentPattern = 1;                               //this handles wrapping back around to beginning of patterns.
        }
        lastJoystickUpdate = 0;                             //reset the timer that tracks the last time the joystick was updated
      } else if (lastJoystickUpdate >= 200 && joystickValues.radial <= -90) {                      //if it's been 200ms since last update and joystick is pushed all the way down
        currentPattern--;
        if (currentPattern < 1) {
          currentPattern = sizeof(patterns)/sizeof(patterns[0]);   //this handles wrapping up to the top end of the array that holds the patterns
        }
        lastJoystickUpdate = 0;

      } else if (lastJoystickUpdate >= 200 && (joystickValues.angular >= 90 || joystickValues.angular <= -90)) {  //if the joystick was pushed all the way to the left
        autoMode = false;                                                                                         //switch to manual mode
        lastJoystickUpdate = 0;   
      }   
    }
    #pragma endregion SelectionMode
  }
}



/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
Miscellaneous functions. Currently this region only includes the function for reading the values of the joystick.
*/
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma region MiscFunctions

/**
 * @brief Reads the analog values from the joystick potentiometers and returns them as a Positions struct.
 *
 * This function reads the analog input values from the joystick's potentiometers on the specified pins,
 * maps the values to a range of -100 to 100 for the angular axis and 100 to -100 for the radial axis, 
 * and applies a deadband to eliminate small fluctuations around the center.
 *
 * @return Positions - a struct containing the mapped and processed values of the angular and radial joystick positions.
 * 
 * The deadband ensures that values near the center of the joystick are treated as zero to account for 
 * measurement noise and prevent unintended small movements.
 */
Positions readJoystick(void) {
  Positions values;
  values.angular = map(analogRead(JOYSTICK_A_PIN), 0, 1023, -100, 100);
  values.radial = map(analogRead(JOYSTICK_R_PIN), 0, 1023, 100, -100);

  if (values.angular <= 5 && values.angular >= -5) values.angular = 0;   //apply a deadband to account for measurement error near center.
  if (values.radial <= 5 && values.radial >= -5) values.radial = 0;
  return values;
}

#pragma endregion MiscFunctions




/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
This region of code contains all the functions related to calculating and performing the motion of the gantry. 
*/
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma region Motion

/**
 * @brief Calculates the effective radial change, accounting for the motion of the angular axis.
 *
 * The radial axis movement is influenced by the angular axis movement, so this function computes the 
 * actual change in the radial axis by considering the steps taken by both the angular and radial motors.
 * 
 * @param angularMoveInSteps The number of steps the angular motor has moved.
 * @param radialMoveInSteps The number of steps the radial motor has moved.
 * 
 * @return int The effective radial change in steps, with the angular axis movement accounted for. 
 *         A positive value indicates a decrease in radius, while a negative value indicates an increase in radius.
 */
int calcRadialChange(int angularMoveInSteps, int radialMoveInSteps) {
  int actualChangeR = angularMoveInSteps - radialMoveInSteps;

  //should return the number of steps R axis has moved, with A axis motion accounted for.
  //if actualChangeR is positive, radius is decreasing. 
  //if actualChangeR is negative, radius is increasing.
  return actualChangeR;          
}


/**
 * @brief Moves both the angular and radial motors to their target positions simultaneously.
 *
 * This function performs relative movements of the motors by taking in the number of steps
 * required for each motor to reach its target. One motor will move at maximum speed, while the 
 * speed of the other motor is scaled to ensure both motors reach their target positions at the 
 * same time. Note that this is a blocking function, meaning no other code will execute while 
 * the motors are moving.
 *
 * @param angularSteps The number of steps the angular motor needs to move to reach its target.
 * @param radialSteps The number of steps the radial motor needs to move to reach its target.
 * 
 * The function adjusts the speed of the motors proportionally based on the distance each motor 
 * needs to travel, ensuring they complete their movements simultaneously. It also reduces the 
 * maximum speed of the angular motor based on the current radial position to avoid excessive 
 * speed at the outer edges.
 *
 * The function checks the state of the run/stop button during execution to allow for immediate 
 * termination of the movement if needed.
 */
void moveToPosition(long angularSteps, long radialSteps) {
  long absStepsA = abs(angularSteps), absStepsR = abs(radialSteps);           //absolute values used to compare total distance each motor travels
  float maxSpeedA = MAX_SPEED_A_MOTOR, maxSpeedR = MAX_SPEED_R_MOTOR;
  float moveTime = 0.0;
  
  // Reduce the maximum angular speed based on the radial position of the ball.
  // If you don't do this, the ball moves too fast when it's on the outer edge of the sand tray.
  float speedReducer = ANGULAR_SPEED_SCALAR * currentPositions.radial;  
  maxSpeedA = MAX_SPEED_A_MOTOR - speedReducer;                         
  
  float speedA = maxSpeedA, speedR = maxSpeedR;              //one of the motors will eventually be moved at max speed, the other will be slowed down.

  //determine which motor has a shorter move, and slow it down proportional to the ratio of the distance each motor travels.

  if ((absStepsA > absStepsR) && (absStepsA != 0)) {         //if Angle motor is moving farther. the second conditional avoids a divide by zero error.
    moveTime = (float)absStepsA / maxSpeedA;                 //how long it will take to move A axis to target at top speed.
    speedR = (float)absStepsR / moveTime;                    //recalculate speed of R motor to take same time as A motor move. Slows down R motor.

  } else if ((absStepsR > absStepsA) && (absStepsR != 0)) {
    moveTime = (float)absStepsR / maxSpeedR;                 //Radial is moving farther. Time in seconds to make that move at max speed.
    speedA = (float)absStepsA / moveTime;                    //Slow down A to complete its move in same amount of time as R.
  }

  //set up the moves for each motor
  stepperAngle.move(angularSteps);       //set up distance the motor will travel in steps. This value can be positive or negative: the sign determines the direction the motor spins.
  stepperAngle.setSpeed(speedA);         //call this to ensure that the motor moves at constant speed (no accelerations).
  stepperRadius.move(radialSteps);
  stepperRadius.setSpeed(speedR);


  //execute steps at the correct speed as long as a motor still needs to travel, and as long as the run/stop
  //button has not been pressed. If the runPattern flag is false, this loop will immediately exit,
  //leaving steps unfinished in the targeted move. There is code in the main loop after the call to moveToPosition()
  //that deals with this.

  //this is a blocking section. The only thing that can happen here is moving the motors and updatting the button state.
  //Adding more functionality inside this loop risks losing synchronization of the motors.
  while (((stepperAngle.distanceToGo() != 0) || (stepperRadius.distanceToGo() != 0)) && runPattern) {     
    stepperAngle.runSpeedToPosition();                             //constant speed move, unless the target position is reached.
    stepperRadius.runSpeedToPosition();
    button.tick();                                                 //This blocking loop can potentially last a long time, so we have to check the button state.
  }
}




/**
 * @brief Performs crash homing on the radial axis at startup.
 *
 * This function moves the radial axis to its home position by driving the motor past the known range 
 * to ensure a hard stop at the mechanical limit. It allows the homing process to be interrupted early 
 * by a long press of the joystick button if the ball reaches the center of the sand garden.
 *
 * @details The function moves the radial axis at a high speed without acceleration to reduce torque 
 * when it reaches the mechanical stop. During the homing sequence, the function updates the LED display 
 * and checks for a long press of the joystick button to potentially terminate the homing process early. 
 * After reaching the stop, the function retracts the motor slightly to create a soft stop, releases any 
 * tension in the mechanism, and sets the current motor position as the origin (0,0).
 *
 * @note This function sets the current position of both the angular and radial motors to zero after homing.
 *
 * @return void
 */
void homeRadius() {
  stepperRadius.move(1.1 * ACTUAL_LEN_R_STEPS);                       //Longer than actual length of axis to ensure that it fully crash homes.
  stepperRadius.setSpeed(600.0);                                      //move fast without accelerations so that the motor has less torque when it crashes.
  while (stepperRadius.distanceToGo() != 0 && !buttonLongPressed) {   //run the R axis toward 0 for the entire length of the axis. Crash homing.
    stepperRadius.runSpeedToPosition();                               //non-blocking move function. has to be called in while loop.
    display.homingSequence(false);                                    //display the homing sequence pattern on the LEDs
    button.tick();                                                    //poll the button to see if it was long pressed
  }
  buttonLongPressed = false;
  stepperRadius.stop();

  delay(100);                                                     //brief delay.
  
  stepperRadius.move(-1 * (HOMING_BUFFER + RELAXATION_BUFFER));   //move away from 0 to create a soft stop. RELAXATION_BUFFER releases tension in bead chain/flexible structures
  stepperRadius.runToPosition();                                  //blocking move.

  stepperRadius.setCurrentPosition(0);                            //set the current positions as 0 steps.
  stepperAngle.setCurrentPosition(0);                             //The current locations of the motors will be the origins of motion.

  currentPositions.angular = 0;                                   //set the global current position variables to 0.
  currentPositions.radial = 0;
  display.homingSequence(true);                                   //now that homing is done, display the homing complete sequence on the LEDs
}



/**
 * @brief Calculates the shortest path to the target position on the angular axis.
 *
 * This function determines the shortest distance required to move from the current position to the 
 * target position on a circular axis, considering both clockwise and counterclockwise directions. 
 * It returns the shortest distance, taking into account a wraparound value for circular motion.
 *
 * @param current The current position on the angular axis, in steps.
 * @param target The desired target position on the angular axis, in steps.
 * @param wrapValue The wraparound point for the axis (e.g., the total number of steps per revolution).
 * 
 * @return int The shortest distance, in steps, required to move to the target position. 
 *         Positive values indicate clockwise movement, while negative values indicate counterclockwise movement.
 */
int findShortestPathToPosition(int current, int target, int wrapValue) {
  int dist1 = modulus((target - current), wrapValue);       
  int dist2 = -1 * modulus((current - target), wrapValue);
  if (abs(dist1) <= abs(dist2)) {
    return dist1;
  } else {
    return dist2;
  }
}



/**
 * @brief Calculates the number of steps required for the radial axis motor to move, accounting for the angular axis motion.
 *
 * This function computes the necessary steps for the radial axis motor to move from the current position 
 * to the target position. It compensates for the fact that the angular axis motion influences the radial 
 * axis but not vice versa. The function adjusts the radial movement based on the planned angular axis movement.
 *
 * @param current The current position of the radial axis in steps.
 * @param target The desired target position of the radial axis in steps.
 * @param angularOffsetSteps The number of steps the angular axis motor will move in the next planned move.
 * 
 * @return int The total number of steps the radial axis motor needs to move, adjusted for the angular axis offset.
 */
int calcRadialSteps(int current, int target, int angularOffsetSteps) {
  return ((current - target) + angularOffsetSteps);
}


/**
 * @brief Manages the entire process of moving both the angular and radial motors to their target positions.
 *
 * This function is responsible for coordinating the motion of both motors, ensuring that the angular 
 * values wrap correctly, that the radial target stays within the defined limits, and that the radial 
 * movement compensates for any angular axis movement. It encapsulates the series of steps required to 
 * calculate the necessary movements, execute them, and update the current positions.
 *
 * @param currentPositions The current position of both the angular and radial axes, represented as a Positions struct.
 * @param targetPositions The desired target position for both the angular and radial axes, represented as a Positions struct.
 * 
 * @return Positions The updated current positions of both the angular and radial axes after the motion has been executed.
 *
 * This function wraps the angular target around the 360-degree transition point and calculates the shortest path 
 * to the target. It also ensures that the radial position stays within its limits, compensates for the mechanical 
 * relationship between the axes, and updates the current position after the move. If the move is interrupted (e.g., 
 * by a long joystick press), the current position tracking adjusts accordingly.
 */
Positions orchestrateMotion(Positions currentPositions, Positions targetPositions) {
  //First take care of making sure that the angular values wrap around correctly,
  targetPositions.angular = modulus(targetPositions.angular, STEPS_PER_A_AXIS_REV);                                                 //wrap value around the 360 degree/0 degree transition if needed
  targetPositions.angular = findShortestPathToPosition(currentPositions.angular, targetPositions.angular, STEPS_PER_A_AXIS_REV);    //Find the shortest path to the new position.

  //First make sure the radial position target won't exceed the limits of the radial axis:
  targetPositions.radial = constrain(targetPositions.radial, 0, MAX_R_STEPS);

  //Update the radial target position based on how much the angular position is going to move.
  //This compensates for the mechanical link between the two axes. This also converts the absolute radial coordinate
  //into a relative coordinate, which stores how many steps the radial motor has to spin. 
  targetPositions.radial = calcRadialSteps(currentPositions.radial, targetPositions.radial, targetPositions.angular); 

  //execute the moves. This is a blocking function: it doesn't return until the move is complete.
  //Also note that these positions are relative coordinates. The pattern function generates an 
  //absolute position as the target to move to, and then the lines of code after that calculate
  //how far the motors have to move in steps to get there. moveToPosition() takes those motor 
  //steps as its arguments. So this function just tells the motors how far they have to move.
  moveToPosition(targetPositions.angular, targetPositions.radial);    


  //Update the current position.
  //moveToPosition can be exited before the move is complete by long pressing the joystick button, so we have
  //to make sure that our position tracking system accounts for that. We also have to use the target positions
  //to update the current position.
  targetPositions.angular -= stepperAngle.distanceToGo();
  targetPositions.radial -= stepperRadius.distanceToGo();
  currentPositions.angular += targetPositions.angular;
  currentPositions.angular = modulus(currentPositions.angular, STEPS_PER_A_AXIS_REV); //wrap the anglular position around if it needs it. 
  currentPositions.radial += calcRadialChange(targetPositions.angular, targetPositions.radial);

  return currentPositions;
}

#pragma endregion Motion




/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
Geometry Generation.
Functions that handle generating points and shapes for drawing. Draw straight lines, create polygons, perform the basic geometric transformations
like rotation, translation, scaling, and (eventually) reflection.
*/
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma region GeometryGeneration

/**
 * @brief Precomputes and returns points approximating a straight line between two positions in polar coordinates.
 *
 * This function precomputes an array of points that approximate a straight line by interpolating between two 
 * end points specified in cartesian coordinates, then converts them to polar coordinates. It stores these points 
 * in a static array and returns the next point on each function call, allowing efficient streaming of precomputed 
 * line points. The line is divided into a specified number of segments (resolution), with a maximum of 100 points.
 *
 * @param point0 The starting point of the line, specified in radial and angular steps.
 * @param point1 The ending point of the line, specified in radial and angular steps.
 * @param current The current position of the gantry, used to calculate relative motion if needed.
 * @param resolution The number of segments to divide the line into, defaulting to 100 and capped at 100.
 * @param reset Bool - set true to force recalculation for a new line.
 *
 * @return Positions The next point along the precomputed line, with radial and angular values in steps.
 *
 * @note The function handles vertical lines by temporarily rotating the points 90 degrees to avoid calculation 
 * issues, then rotates them back before returning. The line is broken into segments up to the maximum length of 
 * the array, and lines close to the center of the field are handled with a higher resolution to maintain accuracy.
 *
 * @details The first call to this function precomputes all points along the line, and subsequent calls return 
 * each point in sequence. The function resets for a new line after the last point is returned.
 */
Positions drawLine(Positions point0, Positions point1, Positions current, int resolution = 100, bool reset = false) {
  //this is the nested array that will store the precomputed points. has to be static so values persist between function calls.
  //it will be of the form pointArray[100][2] = {{r0, theta0}, {r1, theta1}, ... {r99, theta99}}.
  //to access the theta value for point3 (4th point in array), you would call point3.angular = pointArray[3][1];

  //Future update: make this a single layer array of type Positions instead of type Int for simplicity.
  static int pointArray[100][2];  

  static int numPoints = 0;                           //the number of points the line will be approximated with.
  static bool newLine = true;                         //used to track if the function is being called for a new line, or if it needs to provide points for an extant line
  static float x0 = 0, x1 = 0, y0 = 0, y1 = 0;        //end points of the line
  static float xtemp = 0, ytemp = 0, thetaTemp = 0.0; //temporary storage for calculations
  static float stepover = 0;                          //how far to move along x-axis for interpolating along line
  static float m = 0;                                 //the slope of the line (y = mx + b)
  static float denom = 0;                             //the denominator in the slope calculation (x1 - x0)
  static float b = 0;                                 //the y-intercept of the line (y = mx + b)
  static bool pointsRotated = false;                  //used to indicate if points have been rotated to deal with vertical lines and need to be rotated back on output.

  Positions p0 = point0, p1 = point1;                 //containers for the points (we may need to modify their values to deal with vertical lines)
  Positions outputPoint;                              //the struct we'll use for passing the target positions out of the function
  static int outNum = 0;                              //used for tracking which point to return on each call to this function
  
  if (newLine || reset) {                             //if this is a new line, or the reset flag is set
    numPoints = constrain(resolution, 0, 100);     //we can approximate the line with up to 100 points. recalculate this number for each new line.
    
    //check now to see if there will be a vertical line after the coordinate transformation from polar to rectangular coords
    int comparisonA = STEPS_PER_A_AXIS_REV - max(p0.angular, p1.angular);        //units are in steps
    int comparisonB = min(p0.angular, p1.angular);

    //this next step checks to see if the line connecting these two points is within half a degree of vertical in the rectangular coordinate system.
    //From my early testing, if the lines are more than half a degree off of vertical, they render perfectly fine without special handling.
    //It's really just a vertical line that gets weird (e.g., a line connecting two points that are 45 and 315 degrees off the origin ray at the same radius).
    if ((comparisonA - comparisonB <= convertDegreesToSteps(0.5)) && (comparisonA - comparisonB >= convertDegreesToSteps(-0.5))) {
      pointsRotated = true;   //we're going to rotate the points by 90 degrees to deal with the nearly vertical line, so set this flag.
      p0.angular += convertDegreesToSteps(90);
      p1.angular += convertDegreesToSteps(90);
    }

    //take in the points, convert them to radians for the angular unit. only need to do this one time for a new line.
    //also convert each point from polar to cartesian coordinates.
    x0 = p0.radial * cos(convertStepsToRadians(p0.angular));        //x = r*cos(theta)
    y0 = p0.radial * sin(convertStepsToRadians(p0.angular));        //y = r*sin(theta)
    x1 = p1.radial * cos(convertStepsToRadians(p1.angular));        //x = r*cos(theta)
    y1 = p1.radial * sin(convertStepsToRadians(p1.angular));        //y = r*sin(theta)

    denom = x1 - x0;

    //calculate the slope
    m = (y1 - y0) / denom;
    //calculate the y-intercept   y = mx + b, so b = y - mx. Use point0 values for y and x
    b = y0 - (m * x0);


    if (b < 100.0 && b > -100.0) {      //if the line is within 100 steps of the origin
      //This takes care of lines that come really close to intercepting the origin. First, I'm using this range of values rather 
      //than saying if (b == 0.0) because this is using floating point math, and equalities like that almost never evaluate to
      //true with floats. Lines that come really close to the origin require the gantry to flip around 180 degrees in the
      //angular axis once the ball is at the center of the field. The straight line algorithm already handles this well, but if
      //the line is broken into a small number of segments, that large rotation at the center winds up drawing a small arc 
      //around the center. I dealt with this by just having the program maximize the number of segments the lines is broken
      //into for lines which come close to the center. You can adjust the values in the condition above to change what it means
      //for a line to be close to the center to fine tune how well straight lines are drawn.
      numPoints = 100;
    } 
    //This line doesn't come really close to intersecting the origin, so we'll handle it differently.
  
    //divide one axis into the number of segments required by resolution, up to a maximum of the length of the array they'll be stored in.
    //defining all of these values as static means the value will persist between function calls, but also means I have to reset them
    //to initial values once the last point in the line is returned.
    stepover = (x1 - x0) / (float)numPoints;       //should define how far to move along x axis for interpolation along line.

    for (int i = 0; i < numPoints; i++) {
      //start by generating absolute position values for the points along the line in terms of {r, theta}.
      //We are starting with absolute values because the end points of the line are specified in absolute coordinates.

      if (i == 0) {                                             //if it's the first point in the line, put the point0 values into the list to ensure we start there
        pointArray[i][0] = p0.radial;                       //these units are already in steps as absolute coordinates
        pointArray[i][1] = p0.angular;                      //units in steps, absolute coordinates. need to be changed to relative later.
      } else if (i == numPoints - 1) {                          //If it's the last point in the line, put point1 values into the list to make sure we end there.
        pointArray[i][0] = p1.radial;
        pointArray[i][1] = p1.angular;
      } else {                                                  //We're somewhere along the line that isn't the beginning or end, so we need to generate these values.
        //Calculate the next x value in the series. Use the values of i and stepover to figure out how many line segments to increment along from the starting point.
        //I'm using (i + 1) instead of i in the calculation because I'm handling the first and last points separately,
        //so by the time we get to this code, we need to move over by at least one increment of stepover, but i starts counting from 0.
        xtemp = x0 + (i + 1) * stepover;                              
        ytemp = m * xtemp + b;                                  //y = mx + b gives next y value in the series.

        //calculate the angular position of the current point.
        //atan2f(y, x) is a special version of the arctan function that returns the angle based on y and x.
        thetaTemp = atan2f(ytemp, xtemp); 

        //ata2f() has a range of (-pi, pi), and we'll need to shift that to be (0, 2pi)
        if (thetaTemp < 0) thetaTemp = 2.0 * PI + thetaTemp;    //this is in radians, ranging from 0 to 2pi

        //now that we know the anglular position of the point in radians, we need to find the radial position in units of steps
        //using the Pythagorean theorem (square roots calculate more efficiently than trig functions on Arduino Nano).
        //Then store the r and theta points in the array.
        pointArray[i][0] = sqrt(xtemp * xtemp + ytemp * ytemp); //the radial value of the point. This is absolute coordinates from the origin. Units are steps.
        //store the angular position converted from radians to steps. This is still in absolute coordinates, not relative.
        pointArray[i][1] = convertRadiansToSteps(thetaTemp);    
      }
      
      //finally, if we rotated the points to deal with a vertical line, rotate them back.
      if (pointsRotated) {
        pointArray[i][1] -= convertDegreesToSteps(90);
      }
    }

    //we need to set the newLine flag to false so that the next time this function is called,
    //we can output the points along the line rather than recalculating the points.
    newLine = false;       //later in the program, we have to reset this to true once the last line of the point is returned.
    reset = false;
    outNum = 0;            //need to reset this to 0 so we can start outputting the points, starting from the first one.
    pointsRotated = false;
  }
  
  //now we need to output the correct point in the array.
  if (outNum < numPoints) {
    outputPoint.radial = pointArray[outNum][0];   //put the r value into the struct
    outputPoint.angular = pointArray[outNum][1];  //put the theta value into the struct
    outNum++;                                     //increment to the next point in the array
  }

  //once the last point is ready for return, reset all the variables necessary to rerun all the calculations on the next call to this function.
  if (outNum >= numPoints) {
    newLine = true;
  }

  //finally, return the value of the point to be moved to!
  return outputPoint;
}


/**
 * @brief Generates the vertices of a regular n-sided polygon (n-gon) and stores them in an array of Positions.
 *
 * This function computes the vertices of a regular polygon (n-gon) with a specified number of sides, radius, 
 * center point, and optional rotation. The vertices are generated in polar coordinates, with the first vertex 
 * starting at angle 0 (or rotated by the specified degrees) and are then translated to be centered around the 
 * specified center point. The generated points are stored in the provided pointArray.
 *
 * @param pointArray A pointer to the array of Positions to be filled with the vertices of the polygon.
 * @param numPoints The number of vertices (or sides) of the polygon.
 * @param centerPoint The center point of the polygon, specified as a Positions struct (radial and angular coordinates).
 * @param radius The radius of the polygon, which is the distance from the center to each vertex (in motor steps).
 * @param rotationDeg An optional rotation of the polygon in degrees, defaulting to 0.0. This rotates the polygon around its center.
 *
 * @return void
 *
 * @note The function first generates the vertices centered on the origin in polar coordinates, then translates 
 * them to the specified center point by converting to rectangular coordinates, performing the translation, and 
 * converting back to polar. The translatePoints() function is used to handle this translation process.
 *
 * @example
 * // Example of generating an octagon with a radius of 4000 steps centered on the origin:
 * int numberVertices = 8;
 * Positions vertices[numberVertices];
 * Position center = {0, 0};
 * nGonGenerator(vertices, numberVertices, center, 4000, 0.0);
 *
 * // Example of generating a circle with 360 points and a radius of 2000 steps, centered at {3000, 60 degrees}:
 * int numberVertices = 360;
 * Positions vertices[numberVertices];
 * Position center = {3000, convertDegreesToSteps(60)};
 * nGonGenerator(vertices, numberVertices, center, 2000, 0.0);
 */
void nGonGenerator(Positions *pointArray, int numPoints, Positions centerPoint, int radius, float rotationDeg = 0.0) {
  //*pointArry is the pointer to the array that will be built out.
  //numPoints is the length of that array (equal to number of desired vertices).
  //centerPoint is the center point of the polygon (supply as a Position struct)
  //radius is the distance from the center point to a vertex. Units are motor steps.
  //rotationDeg rotates the polygon in degrees. The first vertex will always be at angle = 0, unless you specify a rotation angle.

  //Start by generating vertices in polar coords, centered on origin. 
  int angleStep = STEPS_PER_A_AXIS_REV / numPoints;      //calculate how much to step the angle over for each point

  for (int i = 0; i < numPoints; i++) {
    //define each vertex.
    //What i have done below is the same as doing:
    //pointArray[i].radial = radius; pointArray[i].angular = i * angleStep + convertDegreesToSteps(rotationDeg);
    //This is called aggregate initialization.

    pointArray[i] = {radius, i * angleStep + (int)convertDegreesToSteps(rotationDeg)};
  }

  //Currently all the points in the array are centered on the origin. We need to shift the points to be centered on the
  //desired center point. You can do this in polar coordinates, but it's much simpler to convert to rectangular coordinates,
  //move all the points, and then convert back to polar.
  
  if (centerPoint.radial != 0) {        //if the radial coordinate of the center point is not 0, we need to translate the polygon
    translatePoints(pointArray, numPoints, centerPoint);      //This moves all points in the array to be centered on the correct point
  }
}




/**
 * @brief Translates an array of points along a given translation vector, shifting their position in polar coordinates.
 *
 * This function translates the points in the pointArray by converting both the points and the provided 
 * translation vector from polar to rectangular coordinates, performing the translation, and then converting 
 * the points back to polar coordinates. It is useful for shifting polygons or target positions by a specified 
 * offset. For example, this function can be used to shift the center of a polygon generated by nGonGenerator().
 *
 * @param pointArray A pointer to an array of points (of type Positions) representing the points to be translated.
 * @param numPoints The number of points in the array.
 * @param translationVector The translation vector to shift the points by, specified as a Positions struct.
 *
 * @return void - the array is modified directly because it is passed into this function as a pointer.
 *
 * @note The translation is performed by first converting the points and translation vector to rectangular 
 * coordinates (x, y), adding the corresponding components, and then converting the updated points back to 
 * polar coordinates (r, θ). This ensures that the points are translated accurately in both radial and angular 
 * dimensions. The function assumes the angular component of the translation vector is in steps, and the 
 * radial component is in motor steps.
 *
 * @example
 * // Example usage to shift a polygon to a new center:
 * Positions vertices[8];
 * Positions translationVector = {3500, convertDegreesToSteps(45)};
 * nGonGenerator(vertices, 8, {0, 0}, 4000, 0.0);
 * translatePoints(vertices, 8, translationVector);
 */
void translatePoints(Positions *pointArray, int numPoints, Positions translationVector) {
  if (translationVector.angular != 0 || translationVector.radial != 0) {    //desired polygon is not centered on origin, so we need to shift the points.
    for (int i = 0; i < numPoints; i++) {
      float x = pointArray[i].radial * cos(convertStepsToRadians(pointArray[i].angular));
      float y = pointArray[i].radial * sin(convertStepsToRadians(pointArray[i].angular));

      //now figure out where the center point is in rectangular coordinates
      //NOTE: at some point I want to move this calculation out of the for loop for efficiency
      float centerX = translationVector.radial * cos(convertStepsToRadians(translationVector.angular));
      float centerY = translationVector.radial * sin(convertStepsToRadians(translationVector.angular));

      //now use centerX and centerY to translate each point.
      x += centerX;      //this should shift the X coordinate appropriately
      y += centerY;     //this should shift the Y coordinate appropriately

      //now convert back into polar coordinates

      //calculate the angular position of the current point. Units are in radians.
      //atan2f(y, x) is a special version of the arctan function that returns the angle based on y and x.
      float angleTemp = atan2f(y, x); 

      //atan2f() has a range of (-pi, pi), and we'll need to shift that to be (0, 2pi)
      if (angleTemp < 0) angleTemp = 2.0 * PI + angleTemp;    //this is in radians, ranging from 0 to 2pi

      //now that we know the anglular position of the point in radians, we need to find the radial position in units of steps
      //using the Pythagorean theorem (square roots calculate more efficiently than trig functions on Arduino Nano).
      //Then store the r and theta points in the array.
      pointArray[i].radial = round(sqrt(x * x + y * y));   //the radial value of the point. This is absolute coordinates from the origin. Units are steps.
  
      //store the angular position converted from radians to steps. This is still in absolute coordinates.
      pointArray[i].angular = convertRadiansToSteps(angleTemp);  
    }  
  }
}


/*
NOT IMPLEMENTED.

The idea of this function is to take in an array of points that represent a shape, like a
hexagon generated by nGonGenerator, and use it to scale it up or down in size. I don't 
yet have a great idea of how to solve this problem, so I left it here as a suggestion for 
the hacker. Try your hand at solving this problem!
*/
void scalePoints (Positions *pointArray, int numPoints, float scaleFactor) {

}


/*
NOT IMPLEMENTED.

The idea of this is totake in an array of points, such as one created by nGonGenerator,
and rotate them around an arbitrary point within the drawing field. I had planned to implement this,
but ran out of time, and have left it here as a suggestion for the hacker. Try your hand at
solving this problem!
*/
void rotatePoints(Positions *pointArray, int numPoints, Positions rotationCenter, float rotationDeg) {

}



/*
NOT IMPLEMENTED.

The idea for this is to take in an array of points, like that generated by nGonGenerator,
and to relect it across an arbitrary line in the drawing field. I had planned to implement this,
but ran out of time, and have left it here as a suggestion for the hacker. Try your hand at
solving this problem!
*/
void reflectPoints(Positions *pointArray, int numPoints, Positions reflectionVector) {

}



#pragma endregion GeometryGeneration




/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
This region of code contains useful math functions for doing things like converting between units, doing modulus math that doesn't allow negative
numbers, and finding the distance between points in polar coordinates.
*/
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma region Math


/**
 * @brief Maps a float value from one range to another.
 *
 * This function works similarly to the standard map() function but allows for floating-point inputs 
 * and outputs. It maps a float n from a specified input range (in_min to in_max) to a corresponding 
 * output range (out_min to out_max).
 *
 * @param n The float value to map.
 * @param in_min The lower bound of the input range.
 * @param in_max The upper bound of the input range.
 * @param out_min The lower bound of the output range.
 * @param out_max The upper bound of the output range.
 *
 * @return float The mapped value in the output range.
 */
float fmap(float n, float in_min, float in_max, float out_min, float out_max) {
  return (n - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}


/**
 * @brief Converts an angular measurement in degrees to the corresponding number of steps for a stepper motor.
 *
 * This function converts a given angle in degrees to the number of motor steps required for the stepper motor 
 * to rotate by that angle. The conversion is based on the number of steps per full revolution of the motor.
 *
 * @param degrees The angle in degrees to convert.
 *
 * @return long The number of steps required for the motor to move the specified angle.
 */
long convertDegreesToSteps(float degrees) {
  return round(fmap(degrees, 0.0, 360.0, 0.0, 2.0 * STEPS_PER_MOTOR_REV));
}

/**
 * @brief Converts an angular measurement in radians to the corresponding number of steps for a stepper motor.
 *
 * @param rads The angle in radians to convert.
 * @return long The number of steps required for the motor to move the specified angle in radians.
 */
long convertRadiansToSteps(float rads) {
  return round(fmap(rads, 0.0, 2.0 * PI, 0.0, 2.0 * STEPS_PER_MOTOR_REV));
}

/**
 * @brief Converts a number of steps to the corresponding angle in radians.
 *
 * @param steps The number of motor steps to convert.
 * @return float The equivalent angle in radians.
 */
float convertStepsToRadians(float steps){
  return fmap(steps, 0.0, 2.0 * STEPS_PER_MOTOR_REV, 0.0, 2.0 * PI);
}

/**
 * @brief Converts a number of steps to the corresponding angle in degrees.
 *
 * @param steps The number of motor steps to convert.
 * @return float The equivalent angle in degrees.
 */
float convertStepsToDegrees(int steps) {
  return fmap(float(steps), 0.0, 2.0 * STEPS_PER_MOTOR_REV, 0.0, 360.0);
}


/**
 * @brief Converts a distance in millimeters to the corresponding number of steps for the radial axis.
 *
 * @param mm The distance in millimeters to convert.
 * @return int The equivalent number of steps.
 */
int convertMMToSteps(float mm) {               
  return round(mm * STEPS_PER_MM);
}


/**
 * @brief Converts a number of steps to the corresponding distance in millimeters for the radial axis.
 *
 * @param steps The number of motor steps to convert.
 * @return float The equivalent distance in millimeters.
 */
float convertStepsToMM(float steps) {
  return steps * MM_PER_STEP;
}


/**
 * @brief Computes the modulus of two integers, ensuring the result is non-negative.
 *
 * This function is a replacement for the % operator that prevents negative results by wrapping 
 * negative values around to the positive range. It is mainly used for handling angular values 
 * when the gantry wraps from 360 degrees to 0 degrees.
 *
 * @param x The dividend.
 * @param y The divisor.
 * 
 * @return int The modulus result, always non-negative.
 */
int modulus(int x, int y) {
  return x < 0 ? ((x + 1) % y) + y - 1 : x % y;
}


/**
 * @brief Calculates the distance between two points in polar coordinates using the law of cosines.
 *
 * This function computes the distance between two points specified in polar coordinates (radii and angles). 
 * It uses the law of cosines to perform the calculation, assuming the angles are provided in degrees 
 * and the radii in arbitrary units. The returned distance is in the same units as the radii.
 *
 * @param p1 The first point, represented as a Positions struct (with radial and angular values).
 * @param p2 The second point, represented as a Positions struct (with radial and angular values).
 * 
 * @return int The calculated distance between the two points, rounded to the nearest integer.
 */
int calculateDistanceBetweenPoints(Positions p1, Positions p2) {
    return round(sqrt(pow(p1.radial, 2) + pow(p2.radial, 2) - 2 * p1.radial * p2.radial * cos(convertStepsToRadians(p2.angular) - convertStepsToRadians(p1.angular))));
}

#pragma endregion Math




/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/*
This region of code contains the different pattern generating functions.
*/
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma region Patterns



/**
 * @brief Pattern: Simple Spiral. Generates the next target position for drawing a simple inward and outward spiral.
 *
 * This function calculates the next target position for a simple spiral pattern, starting from the current position. 
 * The pattern progresses by incrementally adding small values to the current angular and radial positions. The spiral 
 * moves inward more quickly than it moves outward due to the mechanical relationship between the radial and angular axes.
 *
 * @param current The current position of the gantry, represented as a Positions struct.
 * @param restartPattern A flag that allows for restarting the pattern (not used in this simple version). Defaults to false.
 * 
 * @return Positions The next target position for the motion controller, represented as a Positions struct.
 * 
 * @note The pattern starts from the current position, so if the previous pattern leaves the ball in a specific position, 
 * the spiral will continue from there. The pattern adjusts the radial and angular steps incrementally and reverses 
 * direction when the radial boundaries are reached.
 */
Positions pattern_SimpleSpiral(Positions current, bool restartPattern = false) {                      
  Positions target;                                        //This is where we'll store the value of the next target position.

  const float angleDivisions = 100.0;                      //Going to divide a full revolution into 100ths. "const" because this never changes.
  const int radialDivisions = 10 * (int)angleDivisions;    //Going to divide the radial axis into 1000ths. Try changing the 10 to a different number, like 20.

  //Calculate how many degrees we'll move over in the angular axis for the next step.
  const int angleStep = convertDegreesToSteps(360.0 / angleDivisions);  

  //Calculate how far along we'll move the radial axis for the next step. 
  //The "static" keyword means that this variable is defined once when the function is run for the first time.
  //This is different than "const" because this is a variable, not a constant, so we can still change the value.
  //If the following line were to omit the "static" keyword, this variable would be reset to its initial value
  //every time the function is called, meaning that we couldn't change it between positive and negative to 
  //make the spiral grow inward or outward.
  static int radialStep = MAX_R_STEPS / radialDivisions;                 

  target.angular = current.angular + angleStep;            //Set the angular position of the new point to move to (target position)
  target.radial = current.radial + radialStep;             //Set the radial target position as the current radial position plus the radialStep

  if (target.radial > MAX_R_STEPS || target.radial < 0) {  //Logic to see if the targeted radial position is out of bounds of the radial axis
    radialStep *= -1;                                      //If we're out of bounds, switch the sign of the radialStep (now we move in the opposite direction)
    target.radial += 2 * radialStep;                       //We were already out of bounds, so we undo that by moving 2 radialSteps in the new direction.
  }

  return target;                                           //Return the target position so that the motion control functions can move to it.
}


/**
 * @brief Pattern: Cardioids. Generates the next target position for drawing repeating, slowly rotating cardioids.
 *
 * This function generates the next target position for a cardioid pattern, moving in relative coordinates by adding 43 degrees 
 * to the current angular position and adjusting the radial position by 1/8th of the total radial axis. The pattern alternates 
 * the direction of radial movement, creating a stepped approximation of a triangle wave along the radial axis.
 *
 * @param current The current position of the gantry, represented as a Positions struct.
 * @param restartPattern A flag that allows restarting the pattern, setting the angular and radial positions to 0. Defaults to false.
 *
 * @return Positions The next target position for the motion controller, represented as a Positions struct.
 * 
 * @note This pattern works best after a reset, as it always operates in relative coordinates. If started after running another pattern, 
 * the results may vary, since it builds upon the current position of the gantry.
 */
Positions pattern_Cardioids(Positions current, bool restartPattern = false) {                      
  Positions target;
  const int radialStep = ((MAX_R_STEPS) / 8);       //we're going to take huge steps radially (this defaults to 1/8th of the radial axis)
  static int direction = 1;                         //1 means counterclockwise, -1 means clockwise
  static bool firstRun = true;

  if (firstRun || restartPattern) {                 //if it's the first time we're running the pattern, or if we start it from another pattern
    target.angular = 0;
    target.radial = 0;
    firstRun = false;
  } else {
  
    target.angular = current.angular + convertDegreesToSteps(43);   //add 43 degrees to current position
    
    //this block of code moves the radial axis back and forth in increments that are 1/8th the length of the total radial axis.
    //Basically, this is a stepped approximation of a triangle wave.

    int nextRadial = current.radial + (direction * radialStep);      //calculate potential next radial position

    if ((nextRadial <= MAX_R_STEPS) && (nextRadial >= 0)) {          //If the next radial position is in bounds of the radial axis soft limits
      target.radial = nextRadial;                                    //Moves the radial axis positive direction by 1/8th the length of the axis
    } else {
      direction *= -1;                                               //switch the radial step direction
      target.radial = current.radial + (direction * radialStep);
    }
  }

  return target;
}


/**
 * @brief Pattern: Wavy Spiral. Generates the next target position for drawing a wavy spiral pattern.
 *
 * This function creates a wavy spiral pattern, which is similar to the simple spiral pattern but with an additional sine wave 
 * component added to the radial position. The result is a spiral with oscillating radial movement, creating a wavy effect. 
 * The sine wave's amplitude and frequency can be adjusted to control the wave's characteristics.
 *
 * @param current The current position of the gantry, represented as a Positions struct.
 * @param restartPattern A flag that allows restarting the pattern. Defaults to false.
 * 
 * @return Positions The next target position for the motion controller, represented as a Positions struct.
 * 
 * @note This pattern adds a sine wave to the radial position to create the wavy effect. You can modify the amplitude and frequency 
 * of the wave to achieve different variations of the pattern. The radial movement is reversed when the limits of the radial axis are reached.
 */
Positions pattern_WavySpiral(Positions current, bool restartPattern = false) {

  Positions target;                                  //This is where we'll store the value of the next target position.

  float angleDivisions = 100.0;                      //Going to divide a full revolution into 100ths. "const" because this never changes.
  int radialDivisions = 10 * (int)angleDivisions;    //Going to divide the radial axis into 1000ths. Try changing the 10 to a different number, like 20.
  
  //Add in values for the amplitude and frequency of the sine wave
  float amplitude = 200.0;
  int period = 8;

  //Calculate how many degrees we'll move over in the angular axis for the next step.
  const int angleStep = convertDegreesToSteps(360.0 / angleDivisions);  

  static int radialStep = MAX_R_STEPS / radialDivisions;                 

  target.angular = current.angular + angleStep;            //Set the angular position of the new point to move to (target position)
  target.radial = current.radial + radialStep;             //Set the radial target position as the current radial position plus the radialStep

  if (target.radial > MAX_R_STEPS || target.radial < 0) {  //Logic to see if the targeted radial position is out of bounds of the radial axis
    radialStep *= -1;                                      //If we're out of bounds, switch the sign of the radialStep (now we move in the opposite direction)
    target.radial += 2 * radialStep;                       //We were already out of bounds, so we undo that by moving 2 radialSteps in the new direction.
  }
  
  target.radial += (int)(amplitude * sin(period * convertStepsToRadians(target.angular)));

  return target;                                           //Return the target position so that the motion control functions can move to it.
}



/**
 * @brief Pattern: Rotating Squares. Generates the next target position for drawing rotating squares, each rotated by 10 degrees.
 *
 * This function draws squares of the same size by connecting four points in sequence and rotating the square by 10 degrees 
 * after completing each one. The function uses a switch-case statement to control the drawing process, ensuring each side 
 * of the square is drawn in order. Once a square is complete, the vertices are rotated for the next iteration.
 *
 * @param current The current position of the gantry, represented as a Positions struct.
 * @param restartPattern A flag that allows restarting the pattern. Defaults to false.
 *
 * @return Positions The next target position for the motion controller, represented as a Positions struct.
 * 
 * @note This pattern relies on a static variable to track the current step in the drawing process and uses the drawLine function 
 * to move between the vertices of the square. After each square is completed, the vertices are rotated by 10 degrees for the next square.
 */
Positions pattern_RotatingSquares(Positions current, bool restartPattern = false) {                
  Positions target;             
  static int step = 0;
  static int segments = 20;                         //Use  20 points to approximate a straight line
  static Positions p1, p2, p3, p4;                  //the four vertices of our square
  static bool firstRun = true;                      //used to track if this is the first time the function is called
  const int angleShift = convertDegreesToSteps(10); //how much we'll rotate the square
  if (firstRun || restartPattern) {
    p1.angular = 0;                                 //angular position of first point in absolute coordinates
    p1.radial = 7000;                               //radial position of first point in absolute coordiantes (units are steps)
    p2.angular = convertDegreesToSteps(90);
    p2.radial = 7000;
    p3.angular = convertDegreesToSteps(180);
    p3.radial = 7000;
    p4.angular = convertDegreesToSteps(270);
    p4.radial = 7000;
    firstRun = false;
  }

  switch (step) {
  case 0:                                                                   //if step == 0
    target = drawLine(p1, p2, currentPositions, segments);                  //target positions are the result of calling drawLine between points p1 and p2
    if ((target.angular == p2.angular) && (target.radial == p2.radial)) {   //If we've reached the end of the line
      step++;                                                               //Increment the value of step so we can move on to the next line
    }   
    break;                                                                  //have to include "break;" to avoid case fall through
  
  case 1:                                                                   //if step == 1
    target = drawLine(p2, p3, currentPositions, segments);
    if ((target.angular == p3.angular) && (target.radial == p3.radial)) {
      step++;
    }
    break;
  
  case 2:  
    target = drawLine(p3, p4, currentPositions, segments);
    if ((target.angular == p4.angular) && (target.radial == p4.radial)) {
      step++;
    }
    break;

  case 3:
    target = drawLine(p4, p1, currentPositions, segments);
    if ((target.angular == p1.angular) && (target.radial == p1.radial)) {
      step++;                                                               //incrementing again would take us to case 4, but we don't have that, so default gets called next
    }
    break;
  
  default:
    //assuming that the step number was out of bounds, so reset it
    step = 0;                 //start the square over
    target = current;         //set the target position to the current position just as a default for the default option in the switch statement.
    p1.angular += angleShift; //rotate all points in the square by 10 degrees
    p2.angular += angleShift;
    p3.angular += angleShift;
    p4.angular += angleShift;
    break;  
  }
  
  return target;
}




/**
 * @brief Pattern: Pentagon Spiral. Generates the next target position for drawing a growing and shrinking pentagon spiral.
 *
 * This function creates a pentagon using the nGonGenerator function to generate the vertices and then iterates through 
 * the vertices, connecting them with straight lines. After completing a pentagon, the radius of each vertex is adjusted 
 * by a radial step value (radialStepover). When the radius exceeds the maximum or falls below zero, the direction of 
 * the radial change is reversed, creating a pattern of growing and shrinking pentagons.
 *
 * @param current The current position of the gantry, represented as a Positions struct.
 * @param restartPattern A flag that allows restarting the pattern. Defaults to false.
 * 
 * @return Positions The next target position for the motion controller, represented as a Positions struct.
 * 
 * @note This pattern does not use a switch-case statement for sequencing but instead iterates over a list of precomputed points 
 * (the vertices of the pentagon) and adjusts the radial distance of each point to create a spiral effect. The vertices are 
 * recalculated when a complete pentagon is drawn.
 */

Positions pattern_PentagonSpiral(Positions current, bool restartPattern = false) {
  Positions target;                                                   //Output position will be stored here
  static int start = 0;                                               //Index to the starting point of the line in the array
  static int end = 1;                                                 //Index to the end point of the line in the array
  static bool firstRun = true;                                        //Flag for tracking if a new polygon needs to be generated
  const int vertices = 5;                                             //Change this to make a different polygon
  static Positions vertexList[vertices];                               //construct an array to store the vertices of the polygon
  static int radialStepover = 500;                                    //Amount to change the radius of the polygon each cycle

  if (firstRun || restartPattern) {                                                     //On first function call, construct the polygon vertices
    nGonGenerator(vertexList, vertices, {0,0}, 1000, 0.0);             //generate the vertices of the polygon  
    firstRun = false;                                                 //Use already generated points next time this function is called
  }   
  target = drawLine(vertexList[start], vertexList[end], current, 100);  //draw the line between the appropriate points

  if ((target.angular == vertexList[end].angular) &&                   //If the line is complete, need to move on to the next line
    (target.radial == vertexList[end].radial)) {   
    start++;                                                          //increment start and end points of the line in the array
    end++;            
    start = modulus(start, vertices);                                 //wrap around to beginning of array if needed
    end = modulus(end, vertices);           
    if (start == 0 && end == 1) {                                     //If we're onto a new iteration of the polygon
      for (int i = 0; i < vertices; i++) {                            //Increase or decrease the radius of each point
        int newR = vertexList[i].radial + radialStepover;            
        if (newR > MAX_R_STEPS || newR < 0) {                         //If the radius is getting out of bounds
          radialStepover *= -1;                                       //Switch direction of radial change
          newR += 2 * radialStepover;                                 //move the other way
        }           
        vertexList[i].radial = newR;                                   //change the radius for each point
      }           
    }           
  }           
  return target;                                                      //return the new target position
}





/**
 * @brief Pattern: Hex Vortex. Generates the next target position for drawing a series of growing, shrinking, and rotating hexagons.
 *
 * This function generates a hexagon vortex pattern, where hexagons grow and shrink over time while rotating. 
 * When the outer edge of the radial axis is reached, the ball moves along the rim before shrinking back inward. 
 * The ball also dwells at the center of the field. The pattern is controlled using a switch-case sequence that 
 * moves between the six vertices of the hexagon.
 *
 * @param current The current position of the gantry, represented as a Positions struct.
 * @param restartPattern A flag that allows restarting the pattern. Defaults to false.
 * 
 * @return Positions The next target position for the motion controller, represented as a Positions struct.
 * 
 * @note The hexagon grows and shrinks by adjusting the radius incrementally (radialStepover) and rotates 
 * by shifting the angular positions of each vertex. The pattern reverses direction when the radius exceeds 
 * the maximum limit or falls below zero.
 */
Positions pattern_HexagonVortex(Positions current, bool restartPattern = false) {                 
  Positions target;           
  static int step = 0;                                //using switch case to track steps again
  static int segments = 100;
  static Positions p1, p2, p3, p4, p5, p6;            //vertices of the hexagon
  static bool firstRun = true;
  const int angleShift = convertDegreesToSteps(5);
  static int radialStepover = 350;                    //how much we'll increase or decrease the size of the hexagon each iteration
  static int radius = 1000;                           //starting radius
  
  if (firstRun || restartPattern) {
    p1.angular = 0;
    p1.radial = radius;
    p2.angular = convertDegreesToSteps(60);
    p2.radial = radius;
    p3.angular = convertDegreesToSteps(120);
    p3.radial = radius;
    p4.angular = convertDegreesToSteps(180);
    p4.radial = radius;
    p5.angular = convertDegreesToSteps(240);
    p5.radial = radius;
    p6.angular = convertDegreesToSteps(300);
    p6.radial = radius;
    firstRun = false;
  }

  //the step sequencer works just like the rotating square example, but with more steps
  switch (step) {
  case 0:
    target = drawLine(p1, p2, currentPositions, segments);
    if ((target.angular == p2.angular) && (target.radial == p2.radial)) {
      step++;
    }
    break;
  
  case 1:
    target = drawLine(p2, p3, currentPositions, segments);
    if ((target.angular == p3.angular) && (target.radial == p3.radial)) {
      step++;
    }
    break;
  
  case 2:  
    target = drawLine(p3, p4, currentPositions, segments);
    if ((target.angular == p4.angular) && (target.radial == p4.radial)) {
      step++;
    }
    break;

  case 3:
    target = drawLine(p4, p5, currentPositions, segments);
    if ((target.angular == p5.angular) && (target.radial == p5.radial)) {
      step++;
    }
    break;
  
  case 4:
    target = drawLine(p5, p6, currentPositions, segments);
    if ((target.angular == p6.angular) && (target.radial == p6.radial)) {
      step++;
    }
    break;

  case 5:
    target = drawLine(p6, p1, currentPositions, segments);
    if ((target.angular == p1.angular) && (target.radial == p1.radial)) {
      step++;
    }
    break;

  case 6:
    //reset to the beginning
    step = 0;
    target = current;         //set the target position to the current position just as a default for the default option in the switch statement.

    p1.angular += angleShift; //rotate all points
    p2.angular += angleShift;
    p3.angular += angleShift;
    p4.angular += angleShift;
    p5.angular += angleShift;
    p6.angular += angleShift; 

    if ((radius + radialStepover >= MAX_R_STEPS + 2000) || (radius + radialStepover <= 0)) radialStepover *= -1;    //If we're too far out of bounds, switch directions
    radius += radialStepover;  //increase or decrease the radius for the points

    p1.radial = radius;
    p2.radial = radius;
    p3.radial = radius;
    p4.radial = radius;
    p5.radial = radius;
    p6.radial = radius;

    break;
  
  default:
    //assuming that the step number was out of bounds, so reset it
    step = 0;
    target = current;         //set the target position to the current position just as a default for the default option in the switch statement.
    break;  
  }
  
  return target;
}






/**
 * @brief Pattern: Pentagon Rainbow. Generates the next target position for drawing an off-center pentagon that rotates and moves.
 *
 * This function creates a pentagon pattern that is off-center, moving the center of the pentagon to a new position and 
 * rotating it slightly with each iteration. The pentagon is generated using nGonGenerator and translated to the 
 * appropriate location, while the center and orientation are adjusted progressively.
 *
 * @param current The current position of the gantry, represented as a Positions struct.
 * @param restartPattern A flag that allows restarting the pattern. Defaults to false.
 * 
 * @return Positions The next target position for the motion controller, represented as a Positions struct.
 * 
 * @note The center of the pentagon is translated and rotated slightly on each iteration, creating a "rainbow" effect 
 * as the pentagon appears in different positions. The nGonGenerator and translatePoints functions are used to 
 * generate and move the pentagon.
 */
Positions pattern_PentagonRainbow(Positions current, bool restartPattern = false) {
  Positions target;
  //target = current;               
  static int start = 0;
  static int end = 1;
  static bool firstRun = true;
  const int vertices = 5;  
  static Positions pointList[vertices];
  static int radialStepover = 500;
  const int shiftDeg = 2;
  static int angleShift = convertDegreesToSteps(shiftDeg);
  static int shiftCounter = 1;

  if (firstRun || restartPattern) {
    nGonGenerator(pointList, vertices, {0, 0}, 3000, 0.0);      //create the polygon
    translatePoints(pointList, vertices, {4000, 0});            //move the polygon to the appropriate spot
    firstRun = false;
  } 
  

  target = drawLine(pointList[start], pointList[end], current, 100);

  if ((target.angular == pointList[end].angular) && (target.radial == pointList[end].radial)) {
    start++;
    end++;
    start = modulus(start, vertices);
    end = modulus(end, vertices);
    nGonGenerator(pointList, vertices, {0, 0}, 3000, shiftCounter * shiftDeg);    //build a new polygon that is rotated relative to the previous one
    translatePoints(pointList, vertices, {4000, shiftCounter * angleShift});      //move to the correct point
    shiftCounter++;
  }
  return target;
}



/**
 * @brief Pattern: Random Walk 1. Generates random target positions, moving via the shortest path to each point.
 *
 * This function creates a random walk pattern by generating random target positions in both the radial and angular axes. 
 * The motion controller moves the gantry via the shortest path to each randomly generated point, resulting in random arcs.
 *
 * @param current The current position of the gantry, represented as a Positions struct.
 * @param restartPattern A flag that allows restarting the pattern. Defaults to false.
 * 
 * @return Positions The next randomly generated target position for the motion controller, represented as a Positions struct.
 * 
 * @note This pattern moves the gantry using the shortest path between points, leading to random arc-shaped movements.
 */
Positions pattern_RandomWalk1(Positions current, bool restartPattern = false) {
  Positions target;

  // Generate a random radial position within the bounds of your system.
  int randomRadial = random(0, MAX_R_STEPS + 1); // +1 because the upper bound is exclusive

  // Generate a random angular position within a full circle in steps.
  int randomAngular = random(0, STEPS_PER_A_AXIS_REV);

  // Set the target position to the randomly generated values.
  target.radial = randomRadial;
  target.angular = randomAngular;

  return target;
}


/**
 * @brief Pattern: Random Walk 2. Generates random target positions and moves in straight lines to each point.
 *
 * This function creates a random walk pattern by generating random target positions in both the radial and angular axes. 
 * Unlike Random Walk 1, this version moves the gantry in straight lines to each random point by connecting the current 
 * position to the random target using the drawLine function.
 *
 * @param current The current position of the gantry, represented as a Positions struct.
 * @param restartPattern A flag that allows restarting the pattern. Defaults to false.
 * 
 * @return Positions The next target position for the motion controller, represented as a Positions struct.
 * 
 * @note The function generates new random points once the gantry reaches the current random target and continues the random walk.
 */
Positions pattern_RandomWalk2(Positions current, bool restartPattern = false) {
  Positions target;
  static Positions randomPoint, lastPoint = current;
  static bool makeNewRandomPoint = true;
  
  if (makeNewRandomPoint) {
    // Generate a random radial position within the bounds of your system.
    randomPoint.radial = random(0, MAX_R_STEPS + 1); // +1 because the upper bound is exclusive

    // Generate a random angular position within a full circle in steps.
    randomPoint.angular = random(0, STEPS_PER_A_AXIS_REV);
    makeNewRandomPoint = false;
  }

  // Set the target position to the randomly generated values.
  target = drawLine(lastPoint, randomPoint, current, 100);

  if (target.angular == randomPoint. angular && target.radial == randomPoint.radial) {
    makeNewRandomPoint = true;        //next time we'll generate a new random point
    lastPoint = randomPoint;          //save this as the previous point for the next iteration
  }

  return target;
}



/**
 * @brief Pattern: Accidental Butterfly. Generates the next target position for drawing a butterfly-like pattern with oscillating radial and angular movement.
 *
 * This function creates a butterfly-shaped pattern by modifying a simple spiral pattern with sine and cosine waves that adjust both the radial 
 * and angular positions. The radial and angular positions are oscillated to create the butterfly pattern. I was actually trying to do something
 * entirely different and accidentally made this butterfly.
 *
 * @param current The current position of the gantry, represented as a Positions struct.
 * @param restartPattern A flag that allows restarting the pattern. Defaults to false.
 * 
 * @return Positions The next target position for the motion controller, represented as a Positions struct.
 * 
 * @note The pattern adds sine and cosine-based offsets to both the radial and angular positions to create oscillating movements, leading to the butterfly shape. 
 * The amplitude and frequency of the sine and cosine waves can be adjusted for different effects.
 */
Positions pattern_AccidentalButterfly(Positions current, bool restartPattern = false) {
  //This pattern starts out exactly the same as pattern_SimpleSpiral. The only difference is that after calculating the next position
  //in the spiral, it adds the sine of the current angular position to the radial axis to make a wavy line.

  Positions target;                                        //This is where we'll store the value of the next target position.

  const float angleDivisions = 100.0;                      //Going to divide a full revolution into 100ths. "const" because this never changes.
  const int radialDivisions = 10 * (int)angleDivisions;    //Going to divide the radial axis into 1000ths. Try changing the 10 to a different number, like 20.
  
  //Add in values for the amplitude and frequency of the sine wave
  const float amplitude = 200.0;
  const int frequency = 8;

  //Calculate how many degrees we'll move over in the angular axis for the next step.
  const int angleStep = convertDegreesToSteps(360.0 / angleDivisions);  

  //Calculate how far along we'll move the radial axis for the next step. 
  static int radialStep = MAX_R_STEPS / radialDivisions;                 

  target.angular = current.angular + angleStep;            //Set the angular position of the new point to move to (target position)
  target.radial = current.radial + radialStep;             //Set the radial target position as the current radial position plus the radialStep

  if (target.radial > MAX_R_STEPS || target.radial < 0) {  //Logic to see if the targeted radial position is out of bounds of the radial axis
    radialStep *= -1;                                      //If we're out of bounds, switch the sign of the radialStep (now we move in the opposite direction)
    target.radial += 2 * radialStep;                       //We were already out of bounds, so we undo that by moving 2 radialSteps in the new direction.
  }

  //Add a new component to the radial position to make it oscillate in and out as a sine wave.
  int rOffset = (int)(200.0 * sin(8 * convertStepsToRadians(target.angular)));
  int aOffset = (int)(40.0 * cos(3 * convertStepsToRadians(target.angular)));
  target.radial += rOffset;


  //Now do the same for the angular axis so we get some back and forth:
  target.angular += aOffset;

  return target;        //Return the target position so that the motion control functions can move to it.
}


/**
 * HACK BY ORION (AKA: Magicaldroid)
 *
 * @brief Generates the next target position to follow the points within pointList
 *
 * @param pointList An array of all the points in the pattern stored in a Positions struct.
 * @param current The current position of the gantry, represented as a Positions struct.
 * @param nodes Total number of points in the pattern
 * 
 * @return Positions The next target position for the motion controller, represented as a Positions struct.
 * 
 */
Positions drawPictureStep(Positions *pointList, int nodes, Positions current)
{
  Positions target;
  static int start = 0;
  static int end = 1;
  static bool changePoints = true;
  static Positions readStart = {0,0};
  static Positions readEnd = {pgm_read_word(&(pointList[0].radial)), pgm_read_word(&(pointList[0].angular))};
  
  uint16_t readValueR;
  uint16_t readValueA;

  if (changePoints) {
    readValueR = pgm_read_word(&(pointList[end].radial));
    readValueA = pgm_read_word(&(pointList[end].angular));
    readStart = readEnd;
    readEnd = {readValueR, readValueA};
    changePoints = false;
  }

  Positions startPoint = { (int)(readStart.radial * (float)MAX_R_STEPS/1000), (int)convertDegreesToSteps((float)(readStart.angular)/10) };
  Positions endPoint = { (int)(readEnd.radial * (float)MAX_R_STEPS/1000), (int)convertDegreesToSteps((float)(readEnd.angular)/10) };

  target = drawLine(startPoint, endPoint, current, 100);

  if ((target.angular == endPoint.angular) && (target.radial == endPoint.radial)) {
    changePoints = true;
    start++;
    end++;
    start = start % nodes;
    end = end % nodes;
  }
  return target;
}

/**
 * HACK BY ORION (AKA: Magicaldroid)
 *
 * @brief Pattern: Draw A Picture. Generates the next target position for following the predefined image pattern
 *
 * This function connects the points specified in the array defined within this function
 * This array of points can be generated using the Image-to-Sand tool here
 *
 * @param current The current position of the gantry, represented as a Positions struct.
 * @param restartPattern A flag that allows restarting the pattern. Defaults to false.
 * 
 * @return Positions The next target position for the motion controller, represented as a Positions struct.
 * 
 */
Positions pattern_Picture(Positions current, bool restartPattern = false) {
  static const Positions pointList[] PROGMEM = {
    // Paste in image coordinates here (or uncomment one of the pre-made options below)

    // CrunchLabs Logo
    {996,953},{996,1048},{996,1162},{997,1272},{996,1384},{997,1483},{997,1615},{997,1719},{999,1861},{999,1982},{997,2069},{988,2086},{658,1997},{617,1934},{626,1859},{623,1767},{623,1625},{621,1471},{620,1275},{623,1157},{624,1073},{623,978},{622,857},{622,680},{621,519},{620,391},{599,361},{561,351},{320,996},{298,1072},{233,1703},{239,1834},{279,2482},{321,2598},{536,3239},{589,3242},{620,3209},{623,3106},{625,2934},{627,2758},{627,2642},{617,2571},{645,2513},{680,2486},{986,2410},{998,2416},{1000,2566},{1000,2640},{999,2752},{999,2880},{998,2994},{998,3116},{996,3203},{997,3304},{999,3442},{996,3556},{999,79},{998,202},{997,300},{998,423},{997,507},{996,630},{998,754},{996,869}    

    // Palm tree
    //{906,2993},{846,2869},{754,2758},{612,2665},{438,2612},{581,2487},{678,2375},{719,2277},{739,2154},{724,2092},{612,2170},{585,2159},{480,2249},{513,2078},{525,1944},{521,1675},{330,1897},{323,1538},{331,1367},{322,1242},{202,1408},{177,1382},{127,1738},{107,1753},{154,2259},{256,2627},{73,2960},{202,722},{386,808},{393,839},{346,921},{381,897},{396,925},{435,884},{484,1101},{619,1238},{839,1299},{896,1323},{939,1311},{942,1294},{892,1187},{845,979},{882,749},{914,634},{1000,455},{944,446},{892,466},{697,494},{510,631},{487,584},{459,634},{435,516},{429,672},{127,3528},{285,2745},{307,2788},{335,3037},{344,3204},{377,3226},{417,3337},{444,3326},{539,3439},{569,3272},{564,3122},{536,2972},{470,2806},{551,2832},{644,2933},{673,2927},{743,2963},{763,2952},{852,2991}

    // Mountains
    //{909,1839},{779,1842},{622,1887},{564,1930},{529,1921},{451,2067},{425,2093},{405,2074},{341,2288},{243,2322},{230,2297},{425,1818},{244,2028},{195,2054},{185,2166},{92,2224},{136,2346},{193,2329},{204,2423},{145,2515},{120,2474},{62,3046},{160,2632},{258,2827},{268,2870},{209,3056},{185,3007},{152,3035},{161,2866},{92,3080},{168,3164},{188,3391},{286,3430},{301,3351},{318,3355},{382,3499},{364,3406},{423,3402},{455,3487},{483,3487},{655,55},{664,82},{540,90},{239,3503},{67,321},{122,1760},{308,1599},{387,1666},{627,1662},{652,1653},{659,1630},{522,1525},{403,483},{420,513},{419,676},{446,727},{487,722},{539,654},{683,492},{1000,315},{630,527},{496,692},{449,714},{433,471},{402,461},{442,1469},{572,1574},{642,1631},{631,1648},{455,1658},{353,1634},{307,1577},{124,1685},{47,934},{122,170},{150,393},{191,124},{223,106},{263,3571},{542,101},{691,79},{699,60},{620,23},{488,3462},{460,3461},{428,3382},{356,3392},{309,3301},{322,3116},{313,3030},{349,2876},{284,2727},{217,2392},{360,2292},{365,2201},{427,2151},{498,2000},{526,1995},{674,1876}

    // Fir tree
    //{1000,2678},{872,2631},{736,2463},{693,2447},{624,2480},{558,2233},{512,2209},{453,2268},{440,2259},{520,1869},{512,1838},{487,1818},{413,1854},{397,1835},{634,1602},{649,1580},{615,1553},{470,1522},{475,1499},{796,1436},{830,1420},{828,1406},{781,1377},{582,1280},{438,1065},{590,1017},{669,1019},{739,1042},{743,747},{597,794},{465,805},{424,783},{472,642},{772,409},{831,375},{847,351},{812,339},{435,310},{432,290},{567,243},{633,217},{650,197},{642,177},{346,3544},{362,3523},{469,3564},{504,3545},{514,3503},{398,3096},{412,3088},{487,3166},{524,3153},{587,2875},{607,2874},{647,2912},{691,2898},{795,2769},{880,2719},{980,2692}
    
    // Iceberg
    //{663,154},{535,3533},{456,3522},{571,162},{588,210},{800,771},{689,884},{895,997},{703,1635},{550,1835},{641,1825},{794,1646},{997,1010},{1000,980},{786,873},{892,792},{892,758},{666,221},{855,3470},{840,3447},{688,3442},{513,3344},{610,3030},{604,2990},{499,2889},{739,2693},{758,2663},{614,2508},{654,2472},{660,2425},{577,1995},{792,1962},{920,1914},{930,1888},{892,1869},{752,1913},{599,1904},{460,1987},{281,2026},{182,2344},{118,2940},{245,3230},{366,3429},{524,3437},{665,3507},{818,3496}
  
    // Cat
    //{1000,2323},{915,2299},{942,2251},{946,2200},{920,2158},{926,2115},{914,2096},{887,2086},{879,2077},{821,2056},{751,2068},{727,2068},{696,2056},{661,1984},{670,1924},{669,1863},{625,1749},{601,1689},{561,1578},{570,1477},{597,1438},{686,1378},{758,1332},{774,1333},{815,1352},{838,1352},{873,1325},{868,1299},{896,1294},{922,1276},{924,1267},{890,1223},{853,1189},{811,1174},{751,1184},{731,1173},{731,1155},{699,1063},{753,1072},{795,1063},{815,1043},{795,967},{778,957},{780,921},{790,917},{790,881},{780,877},{800,712},{785,696},{776,641},{751,599},{760,550},{757,475},{736,371},{702,281},{651,180},{600,90},{530,3549},{472,3374},{487,3227},{513,3172},{604,3092},{627,3087},{707,3098},{761,3139},{781,3181},{780,3232},{743,3272},{696,3284},{639,3275},{609,3281},{593,3296},{592,3342},{646,3365},{675,3366},{735,3354},{775,3337},{809,3315},{863,3255},{886,3200},{887,3163},{866,3112},{840,3080},{794,3042},{713,2998},{647,2982},{565,2990},{513,3008},{450,3058},{374,3209},{363,3412},{502,152},{545,217},{574,282},{598,368},{615,463},{620,526},{614,534},{427,350},{341,188},{276,3537},{250,3293},{243,3097},{249,2887},{263,2697},{310,2417},{346,2367},{376,2357},{424,2366},{569,2415},{685,2416},{766,2398},{932,2357},{960,2333},{987,2333}

    // Squirrel
    //{1000,2209},{978,2145},{909,2085},{854,2056},{826,2049},{717,2051},{648,1874},{606,1717},{573,1571},{542,1434},{517,1315},{500,1238},{467,1106},{492,1060},{503,1007},{516,707},{479,682},{446,714},{436,758},{414,761},{380,583},{399,539},{640,506},{745,408},{732,388},{692,384},{638,415},{612,418},{540,377},{496,305},{556,91},{645,59},{742,1},{751,3558},{728,3517},{688,3470},{631,3417},{571,3374},{537,3254},{495,3210},{471,3219},{415,3292},{270,3338},{155,3003},{120,2665},{125,2220},{172,1841},{220,1664},{266,1524},{288,1531},{292,1581},{294,2025},{418,2380},{538,2457},{666,2466},{780,2434},{877,2375},{943,2317},{987,2255}

    // Taj Mahal
    //{760,1834},{747,1797},{765,1788},{803,1597},{915,1460},{938,1466},{942,1449},{1000,328},{990,319},{953,324},{786,3},{799,3572},{764,3511},{746,3509},{696,3563},{866,367},{793,413},{585,16},{566,10},{563,3536},{545,3540},{536,11},{500,3570},{495,54},{472,72},{462,13},{426,3565},{428,3482},{387,3355},{371,3343},{339,3386},{325,3364},{390,3197},{415,3123},{441,2987},{517,2759},{669,2735},{655,2714},{515,2712},{377,2362},{327,2227},{274,2113},{287,2084},{304,2119},{320,2101},{355,1943},{356,1832},{393,1794},{404,1707},{423,1703},{438,1738},{442,1834},{478,1788},{487,1867},{511,1857},{509,1788},{527,1782},{747,1362},{826,1418},{679,1693},{656,1823},{715,1900}

    // Rocket Ship
    //{809,911},{666,1069},{525,1276},{422,1575},{413,1965},{745,2082},{998,2289},{972,2324},{671,2457},{698,2555},{809,2575},{822,2596},{822,2804},{809,2825},{698,2845},{679,2957},{972,3076},{1000,3096},{998,3111},{745,3318},{413,3435},{422,225},{557,580},{695,765},{791,866}
    
    // Intentional Butterfly
    //{1000,1534},{975,1553},{935,1565},{864,1565},{766,1586},{693,1640},{681,1678},{629,1761},{582,1836},{468,1845},{579,1944},{608,2007},{621,2093},{606,2264},{588,2353},{553,2434},{480,2496},{387,2550},{161,2695},{225,2735},{334,2821},{488,2908},{552,2968},{586,3050},{605,3145},{618,3320},{603,3406},{561,3475},{467,3553},{563,3550},{589,3567},{769,213},{939,234},{986,250},{996,281},{947,312},{815,367},{692,413},{476,464},{114,689},{151,796},{150,848},{238,710},{367,699},{219,735},{149,865},{148,941},{248,1060},{352,1027},{268,1068},{142,967},{103,1036},{486,1340},{659,1378},{885,1461},{981,1504}

    // Pi
    //{818,1307},{802,1419},{771,1609},{694,1587},{681,1497},{640,1357},{548,1233},{478,1281},{398,2011},{477,2111},{752,2212},{844,2289},{846,2341},{800,2397},{708,2429},{563,2428},{272,2234},{476,1082},{494,678},{169,3281},{410,2913},{548,2893},{639,2906},{755,2957},{834,3042},{859,3117},{836,3250},{774,3402},{700,3389},{710,3300},{670,3212},{609,3172},{502,3167},{388,3275},{506,446},{596,502},{856,323},{1000,432},{784,1182}

    // Bat Signal    
    //{979,1756},{996,1860},{968,1989},{914,2093},{867,2170},{787,2292},{801,2314},{736,2361},{736,2303},{720,2058},{691,2002},{634,1978},{535,2012},{493,2070},{484,2395},{470,2426},{365,2308},{303,2320},{283,2434},{335,2584},{448,2671},{626,2693},{626,2720},{477,2727},{376,2780},{285,3033},{303,3080},{398,3082},{483,2966},{497,2996},{467,3039},{469,3273},{535,3388},{634,3422},{710,3391},{721,3369},{725,3204},{740,3022},{801,3086},{787,3108},{909,3294},{978,3423},{1000,3523},{980,52},{912,201},{809,401},{710,618},{684,662},{657,652},{670,554},{651,399},{569,316},{420,297},{279,439},{297,647},{619,810},{600,836},{555,831},{523,859},{523,941},{615,963},{619,990},{318,1106},{270,1294},{374,1488},{454,1505},{569,1484},{651,1401},{666,1264},{657,1148},{684,1138},{703,1171},{785,1354},{850,1483},{926,1632}
  
    // More patterns available at https://github.com/orionwc/Image2Sand/blob/main/AdditionalPatterns.txt
  
  };
  
  return drawPictureStep(pointList, sizeof(pointList) / sizeof(pointList[0]), current);

}


#pragma endregion Patterns








/*
NOTES:
  -the max distance the rack can travel is 87.967mm.
  -that's spread over 28 teeth of movement of the pinion.
  -that's 3.141678 mm per tooth. love how close that is to pi.
  -the pinion is 16 teeth. 
  -1 full revolution of the pinion should move the rack 50.267mm.
  -the pulley ratio is 2:1, so two revs of the motor cause one rev of the pinion
  -there are 2048 steps per rev in the motor (may need to update with exact number).
  Note that this is an approximation since the gearbox on the motor is not an integer ratio.
  -4096 steps will drive the pinion one full revolution (so 4096 steps per 50.267mm)
  -that makes 81.485 steps per mm of rack travel. 
  -or the inverse of that if needed: .01227 mm per step
  -to fully move the rack across 87.967mm, need 7167.991 steps. 
  -round down to 7000 steps total, losing about 2mm of total travel, and use 1mm on each end
  as a soft buffer on the travel to prevent crashing.



POSSIBLE IMPROVEMENTS TO MAKE:
- Add backlash compensation. The motor gearboxes have backlash, as do the drive belts and pulleys. Every time the motor
switches direction, it has to spin a bit to take up all the slack before actually moving the gantry. It works fine as it is,
but for greater precision, take direction changes into account and add extra steps to each move to take up the slack before
executing the desired movement. 

- Add functionality for the rest of the geometric transformations: I already added translation, so add rotation, scaling,
and reflection. 

- Add proportional mixing to manual drawing mode. Right now it just works like arrow keys. Mixing would make the control
more subtle and precise.

- Normalize the speed of the ball. Right now the ball travels in a range of speeds, but it would be cool to make a sytem
that would ensure that the ball always moves at the same speed. Requires more advanced path planning than what I have here.
Note that I have a hack in place for this that makes the speed of the angular motor inversely proportional to the radius.
It actually works pretty well, but isn't the same as normalized speed.
*/
