module.exports = [
  { "type": "heading", "defaultValue": "Pebble Fit" },
  {
    "type": "section",
    "items": [
      { "type": "heading", "defaultValue": "Treadmill" },
      {
        "type": "slider",
        "messageKey": "STEPS_PER_MILE",
        "label": "Steps per mile",
        "description": "Used to calculate treadmill distance from your step count.",
        "defaultValue": 1800,
        "min": 500,
        "max": 4000,
        "step": 10
      }
    ]
  },
  {
    "type": "section",
    "capabilities": ["COLOR"],
    "items": [
      { "type": "heading", "defaultValue": "Colors" },
      { "type": "color", "messageKey": "COLOR_BG", "label": "Background", "defaultValue": "000000" },
      { "type": "color", "messageKey": "COLOR_TEXT", "label": "Text (numbers)", "defaultValue": "FFFFFF" },
      { "type": "color", "messageKey": "COLOR_FG", "label": "Foreground (accents)", "defaultValue": "00FF00" }
    ]
  },
  { "type": "submit", "defaultValue": "Save" }
];
