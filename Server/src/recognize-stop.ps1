$ErrorActionPreference = 'Stop'

try {
    Add-Type -AssemblyName System.Speech
    $culture = [System.Globalization.CultureInfo]::GetCultureInfo('fr-FR')
    $recognizer = New-Object System.Speech.Recognition.SpeechRecognitionEngine $culture
    $builder = New-Object System.Speech.Recognition.GrammarBuilder
    $builder.Culture = $culture
    $choices = New-Object System.Speech.Recognition.Choices
    $choices.Add([string[]]@('pause', 'play', 'recommence'))
    $builder.Append($choices)
    $recognizer.LoadGrammar((New-Object System.Speech.Recognition.Grammar $builder))
    $format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(
        16000,
        [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
        [System.Speech.AudioFormat.AudioChannel]::Mono
    )
    [Console]::Out.WriteLine('READY')
    [Console]::Out.Flush()
    $inputStream = [Console]::OpenStandardInput()
    $window = New-Object byte[] 64000
    $hasPrevious = $false
    $windowStart = 0.0
    $lastResultEnd = -1.0
    while ($true) {
        $offset = 32000
        while ($offset -lt $window.Length) {
            $read = $inputStream.Read($window, $offset, $window.Length - $offset)
            if ($read -eq 0) { exit 0 }
            $offset += $read
        }
        if ($hasPrevious) {
            $audio = New-Object System.IO.MemoryStream(,$window)
            $recognizer.SetInputToAudioStream($audio, $format)
            $result = $recognizer.Recognize()
            if ($null -ne $result -and $result.Confidence -ge 0.65) {
                $resultStart = $windowStart + $result.Audio.AudioPosition.TotalSeconds
                if ($resultStart -gt $lastResultEnd) {
                    [Console]::Out.WriteLine($result.Text.ToUpperInvariant())
                    [Console]::Out.Flush()
                    $lastResultEnd = $resultStart + $result.Audio.Duration.TotalSeconds
                }
            }
            $audio.Dispose()
        }
        [Array]::Copy($window, 32000, $window, 0, 32000)
        if ($hasPrevious) { $windowStart += 1.0 }
        $hasPrevious = $true
    }
} catch {
    [Console]::Error.WriteLine($_.ToString())
    exit 1
}
